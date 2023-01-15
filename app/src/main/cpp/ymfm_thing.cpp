#pragma once
#include <jni.h>

// ymfm thing
#include "ymfm.h"

// Oboe thing
#include <oboe/Oboe.h>
using namespace oboe;

#include <android/log.h>

#define LOG_TAG "JNI-ymfm"

#define LOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,  __VA_ARGS__);
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,  __VA_ARGS__);
#define LOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,  __VA_ARGS__);



class MyCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult
    onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {

        // We requested AudioFormat::Float. So if the stream opens
        // we know we got the Float format.
        // If you do not specify a format then you should check what format
        // the stream has and cast to the appropriate type.
        auto *outputData = static_cast<float *>(audioData);

        // Generate random numbers (white noise) centered around zero.
        const float amplitude = 0.2f;
        for (int i = 0; i < numFrames; ++i){
            outputData[i] = ((float)drand48() - 0.5f) * 2 * amplitude;
        }

        return oboe::DataCallbackResult::Continue;
    }
};

class ErrorCallback : public oboe::AudioStreamErrorCallback {
public:
    void onErrorAfterClose(AudioStream *stream, Result error) override {
        LOG_I("%s() - error = %s",__func__,oboe::convertToText(error));
    }
};

std::shared_ptr<oboe::AudioStream> mStream;

extern "C"
JNIEXPORT void JNICALL
Java_team_digitalfairy_ymfm_1thing_YmfmInterface_startOboe(JNIEnv *env, jclass clazz) {
    MyCallback myCallback;
    ErrorCallback errorCallback;

    oboe::AudioStreamBuilder b;

    b.setDirection(oboe::Direction::Output);
    b.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    b.setSharingMode(oboe::SharingMode::Exclusive);
    b.setFormat(oboe::AudioFormat::Float);
    b.setSampleRate(48000);
    //b.setChannelCount(oboe::ChannelCount::Mono);

    b.setDataCallback(&myCallback);
    b.setErrorCallback(&errorCallback);

    oboe::Result r = b.openStream(mStream);

    if(r != oboe::Result::OK) {
        LOG_E("Error: failed to create stream. Error: %s", oboe::convertToText(r));
    } else {
        //LOG_I("Stream Ready.");
    }

    oboe::AudioFormat format = mStream->getFormat();
    LOG_I("AudioStream format is %s", oboe::convertToText(format));

    r = mStream->requestStart();
    if(r != oboe::Result::OK) {
        LOG_E("Error: failed to run Start!. Error: %s", oboe::convertToText(r));

    }

}

// Step 1. load VGM file and prepare YMFM Context
// Step 2. Run down the VGM ticks bound to 44100; yet sound needs to be aligned to 44100
//