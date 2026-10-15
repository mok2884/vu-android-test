// 低遅延モード: Android の AAudio で入力を受け取る (Java から呼ぶ)
#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include <aaudio/AAudio.h>

static AAudioStream *st = NULL;
static float *tmp = NULL;
static int tmpLen = 0;

#define FN(name) Java_jp_mok_vutest_NativeAudio_##name

JNIEXPORT jint JNICALL FN(open)(JNIEnv *e, jclass c, jint devId, jint sr, jint ch, jint preset, jboolean exclusive) {
    AAudioStreamBuilder *b;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    if (devId > 0) AAudioStreamBuilder_setDeviceId(b, devId);
    AAudioStreamBuilder_setSampleRate(b, sr);
    AAudioStreamBuilder_setChannelCount(b, ch);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    // 入力の種類 (加工なし など) は Android 9 から。古い機種では飛ばす
    typedef void (*SetPreset)(AAudioStreamBuilder *, int32_t);
    SetPreset sp = (SetPreset) dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setInputPreset");
    if (sp && preset > 0) sp(b, preset);
    r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { st = NULL; return r; }
    r = AAudioStream_requestStart(st);
    if (r != AAUDIO_OK) { AAudioStream_close(st); st = NULL; return r; }
    return 0;
}

JNIEXPORT jint JNICALL FN(info)(JNIEnv *e, jclass c, jint what) {
    if (!st) return -1;
    switch (what) {
        case 0: return AAudioStream_getChannelCount(st);
        case 1: return AAudioStream_getSampleRate(st);
        case 2: return AAudioStream_getSharingMode(st);      // 0=専有 1=共有
        case 3: return AAudioStream_getPerformanceMode(st);  // 12=低遅延
        case 4: return AAudioStream_getFramesPerBurst(st);
        case 5: return AAudioStream_getBufferSizeInFrames(st);
        case 6: return AAudioStream_getDeviceId(st);
        case 7: return AAudioStream_getXRunCount(st);
    }
    return -1;
}

// frames 個(1chあたり)まで読む。読めた個数を返す
JNIEXPORT jint JNICALL FN(read)(JNIEnv *e, jclass c, jfloatArray arr, jint frames) {
    if (!st) return -1;
    int ch = AAudioStream_getChannelCount(st);
    int need = frames * ch;
    if (need > tmpLen) { free(tmp); tmp = (float *) malloc(sizeof(float) * need); tmpLen = need; }
    aaudio_result_t n = AAudioStream_read(st, tmp, frames, 100000000LL);
    if (n > 0) (*e)->SetFloatArrayRegion(e, arr, 0, n * ch, tmp);
    return n;
}

// out[0]=フレーム位置 out[1]=その時刻(ns, CLOCK_MONOTONIC)
JNIEXPORT jboolean JNICALL FN(timestamp)(JNIEnv *e, jclass c, jlongArray out) {
    if (!st) return JNI_FALSE;
    int64_t pos = 0, t = 0;
    if (AAudioStream_getTimestamp(st, CLOCK_MONOTONIC, &pos, &t) != AAUDIO_OK) return JNI_FALSE;
    jlong v[2] = {pos, t};
    (*e)->SetLongArrayRegion(e, out, 0, 2, v);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL FN(close)(JNIEnv *e, jclass c) {
    if (st) { AAudioStream_requestStop(st); AAudioStream_close(st); st = NULL; }
}
