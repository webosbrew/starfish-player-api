#include "StarfishDirectMediaPlayer.h"

#include <algorithm>
#include <string>
#include <sstream>
#include <functional>
#include <iostream>

#include <pthread.h>

#include <pbnjson.hpp>

namespace pj = pbnjson;

static void StarfishDirectMediaPlayerLoadCallback(gint type, gint64 numValue, const gchar *strValue, void *data);

StarfishDirectMediaPlayer::StarfishDirectMediaPlayer(const std::string &appId) : mAppId(appId)
{
    mStarfishMediaAPIs.reset(new StarfishMediaAPIs());
    mStarfishMediaAPIs->setExternalContext(g_main_context_default());
#if USE_ACB
    mAcbClient.reset(new Acb());
    if (!mAcbClient)
    {
        return;
    }

    auto handler = std::bind(&StarfishDirectMediaPlayer::AcbHandler, this, mAcbClient->getAcbId(),
                             std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
                             std::placeholders::_4, std::placeholders::_5);
    if (!mAcbClient->initialize(ACB::PlayerType::MSE, appId, handler))
    {
        printf("StarfishDirectMediaPlayer.acb initialize failed\n");
        return;
    }
    printf("StarfishDirectMediaPlayer.acb success\n");
#endif
}

StarfishDirectMediaPlayer::~StarfishDirectMediaPlayer()
{
#if USE_ACB
    mAcbClient->setState(ACB::AppState::FOREGROUND, ACB::PlayState::UNLOADED);
    mAcbClient->finalize();
#endif
    mStarfishMediaAPIs.reset(NULL);
}

bool StarfishDirectMediaPlayer::Open(DirectMediaAudioConfig *audioConfig, DirectMediaVideoConfig *videoConfig, const char *windowId)
{
    mAudioConfig = audioConfig;
    mVideoConfig = videoConfig;

    bool ret = false;
    ret = mStarfishMediaAPIs->notifyForeground();

    std::string payload = MakeLoadPayload(0, windowId);
    std::cout << payload << std::endl;
    if (!mStarfishMediaAPIs->Load(payload.c_str(), StarfishDirectMediaPlayerLoadCallback, mStarfishMediaAPIs.get()))
    {
        std::cout << "Load() failed!" << std::endl;
        return false;
    }
    // SetVolume(100);

    std::cout << "Load() succeeded! " << mStarfishMediaAPIs->getMediaID() << std::endl;
    return true;
}

bool StarfishDirectMediaPlayer::Feed(void *data, size_t size, uint64_t pts, DirectMediaFeedType type)
{
    pj::JValue payload = pj::Object();

    std::stringstream convert_invert;
    convert_invert << std::hex << data;

    payload.put("bufferAddr", convert_invert.str());
    payload.put("bufferSize", static_cast<int64_t>(size));
    payload.put("pts", static_cast<int64_t>(pts));
    payload.put("esData", static_cast<int64_t>(type));
    std::string json = pbnjson::JGenerator::serialize(payload, pbnjson::JSchemaFragment("{}"));
    std::string result = mStarfishMediaAPIs->Feed(json.c_str());
    return result.find(std::string("Ok"));
}

size_t StarfishDirectMediaPlayer::Flush()
{
    return mStarfishMediaAPIs->flush();
}

bool StarfishDirectMediaPlayer::SetVolume(int level)
{
    pj::JValue payload = pj::Object();
    payload.put("volume", 100);

    pj::JValue ease = pj::Object();
    ease.put("duration", 0);
    ease.put("type", "Linear");
    payload.put("ease", ease);

    std::string json = pbnjson::JGenerator::serialize(payload, pbnjson::JSchemaFragment("{}"));
    return mStarfishMediaAPIs->setVolume(json.c_str());
}

void StarfishDirectMediaPlayer::Close()
{
    mAudioConfig = nullptr;
    mVideoConfig = nullptr;
    mStarfishMediaAPIs->pushEOS();
}

std::string StarfishDirectMediaPlayer::MakeLoadPayload(uint64_t time, const char *windowId)
{
    pj::JValue payload = pj::Object();
    pj::JValue args = pj::Array();
    pj::JValue arg = pj::Object();
    pj::JValue option = pj::Object();
    pj::JValue contents = pj::Object();
    pj::JValue bufferingCtrInfo = pj::Object();
    pj::JValue externalStreamingInfo = pj::Object();
    pj::JValue codec = pj::Object();
    pj::JValue avSink = pj::Object();
    pj::JValue videoSink = pj::Object();
    videoSink.put("type", "main_video");

    avSink.put("videoSink", videoSink);

    arg.put("mediaTransportType", "BUFFERSTREAM");
    pj::JValue adaptiveStreaming = pj::Object();
    adaptiveStreaming.put("maxWidth", 1920);
    adaptiveStreaming.put("maxHeight", 1080);
    adaptiveStreaming.put("maxFrameRate", 60.000);

    codec.put("video", DirectMediaVideoCodecNames[mVideoConfig->codec]);

    if (mAudioConfig != nullptr)
    {
        pj::JValue audioSink = pj::Object();
        audioSink.put("type", "main_sound");
        avSink.put("audioSink", audioSink);
        codec.put("audio", DirectMediaAudioCodecNames[mAudioConfig->codec]);

        switch (mAudioConfig->codec)
        {
        case DirectMediaAudioCodecOpus:
        {
            pj::JValue opusInfo = pj::Object();
            opusInfo.put("channels", std::to_string(mAudioConfig->channels));
            opusInfo.put("sampleRate", static_cast<double>(mAudioConfig->samplesPerSecond / 1000.f));
            contents.put("opusInfo", opusInfo);
            break;
        }
        case DirectMediaAudioCodecAAC:
        {
            pj::JValue aacInfo = pj::Object();
            aacInfo.put("channels", mAudioConfig->channels);
            aacInfo.put("frequency", static_cast<int>(mAudioConfig->samplesPerSecond / 1000.f));
            aacInfo.put("format", "adts");
            aacInfo.put("profile", 2);
            contents.put("aacInfo", aacInfo);
            break;
        }
        }
    }

    contents.put("codec", codec);

    pj::JValue esInfo = pj::Object();
    esInfo.put("ptsToDecode", static_cast<int64_t>(time));
    esInfo.put("seperatedPTS", true);
    contents.put("esInfo", esInfo);

    contents.put("format", "RAW");
    // contents.put("provider", "Chrome");

    bufferingCtrInfo.put("bufferMaxLevel", 0);
    bufferingCtrInfo.put("bufferMinLevel", 0);
    bufferingCtrInfo.put("preBufferByte", 0);
    bufferingCtrInfo.put("qBufferLevelAudio", 0);
    bufferingCtrInfo.put("qBufferLevelVideo", 0);

    pj::JValue srcBufferLevelAudio = pj::Object();
    srcBufferLevelAudio.put("maximum", 2097152);
    srcBufferLevelAudio.put("minimum", 1024);
    bufferingCtrInfo.put("srcBufferLevelAudio", srcBufferLevelAudio);

    pj::JValue srcBufferLevelVideo = pj::Object();
    srcBufferLevelVideo.put("maximum", 8388608);
    srcBufferLevelVideo.put("minimum", 1024);
    bufferingCtrInfo.put("srcBufferLevelVideo", srcBufferLevelVideo);

    externalStreamingInfo.put("contents", contents);
    // externalStreamingInfo.put("restartStreaming", false);
    // externalStreamingInfo.put("streamQualityInfo", true);
    // externalStreamingInfo.put("streamQualityInfoCorruptedFrame", true);
    // externalStreamingInfo.put("streamQualityInfoNonFlushable", true);
    externalStreamingInfo.put("bufferingCtrInfo", bufferingCtrInfo);

    pj::JValue transmission = pj::JObject();
    transmission.put("contentsType", "WEBRTC");

    option.put("adaptiveStreaming", adaptiveStreaming);
    option.put("appId", mAppId);
    option.put("avSink", avSink);
    // option.put("queryPosition", false);
    option.put("externalStreamingInfo", externalStreamingInfo);
    option.put("lowDelayMode", true);
    option.put("transmission", transmission);
    option.put("needAudio", mAudioConfig != nullptr);
    if (windowId)
    {
        option.put("windowId", windowId);
    }

    arg.put("option", option);
    args.append(arg);
    payload.put("args", args);

    return pbnjson::JGenerator::serialize(payload, pbnjson::JSchemaFragment("{}"));
}

void StarfishDirectMediaPlayer::AcbHandler(long acb_id, long task_id, long event_type, long app_state, long play_state,
                                           const char *reply)
{
    printf("AcbHandler event_type: %d, app_state: %d, play_state: %d, reply: %s\n", event_type, app_state, play_state, reply);
}

static void StarfishDirectMediaPlayerLoadCallback(gint type, gint64 numValue, const gchar *strValue, void *data)
{
    switch (type)
    {
    case PF_EVENT_TYPE_STR_ERROR:
        printf("LoadCallback PF_EVENT_TYPE_STR_ERROR, numValue: %d, strValue: %p\n", numValue, strValue);
        break;
    case PF_EVENT_TYPE_INT_ERROR:
        printf("LoadCallback PF_EVENT_TYPE_INT_ERROR, numValue: %s, strValue: %p\n", numValue, strValue);
        break;
    case PF_EVENT_TYPE_STR_BUFFERFULL:
        printf("PF_EVENT_TYPE_STR_BUFFERFULL\n");
        break;
    default:
        printf("LoadCallback type: %d, numValue: %d, strValue: %p\n", type, numValue, strValue);
        break;
    }
}