#include <jni.h>

// ymfm thing
#include "ymfm.h"

// Oboe thing
#include <oboe/Oboe.h>
using namespace oboe;
#include "handler.h"

#include <android/log.h>

#define LOG_TAG "JNI-ymfm"

#define LOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,  __VA_ARGS__);
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,  __VA_ARGS__);
#define LOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,  __VA_ARGS__);

static YmfmHandler hdr;

extern "C"
JNIEXPORT void JNICALL
Java_team_digitalfairy_ymfm_1thing_YmfmInterface_startOboe(JNIEnv *env, jclass clazz) {

    hdr.open();

}

// Step 1. load VGM file and prepare YMFM Context
// Step 2. Run down the VGM ticks bound to 44100; yet sound needs to be aligned to 44100
//