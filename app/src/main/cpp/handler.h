//
// Created by Alma on 2023/01/15.
//

#ifndef YMFMTHING_HANDLER_H
#define YMFMTHING_HANDLER_H

#include <oboe/Oboe.h>

#include <android/log.h>

#define LOG_TAG "JNI-ymfm"

#define LOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,  __VA_ARGS__);
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,  __VA_ARGS__);
#define LOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,  __VA_ARGS__);


class YmfmHandler {
public:
    oboe::Result open();

    oboe::Result start();

private:

    class MyDataCallback : public oboe::AudioStreamDataCallback {
    public:
        oboe::DataCallbackResult
        onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override;
    };


    class MyErrorCallback : public oboe::AudioStreamErrorCallback {
    public:
        MyErrorCallback(YmfmHandler *parent) : mParent(parent) {}

        virtual ~MyErrorCallback() {}

        void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override;

    private:
        YmfmHandler *mParent;
    };

        std::shared_ptr<oboe::AudioStream> mStream;
        std::shared_ptr<MyDataCallback> mDataCallback;
        std::shared_ptr<MyErrorCallback> mErrorCallback;
};


#endif //YMFMTHING_HANDLER_H
