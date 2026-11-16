// JNI bridge between the Android frontend and the DidjDroid core.
#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>
#include "didj.h"

using namespace didj;

static Didj* g_sys = nullptr;
static std::mutex g_log_mx;
static std::string g_console;   // last ~16 KB of serial console output

static void append_console(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_log_mx);
    g_console += s;
    if (g_console.size() > 16384) g_console.erase(0, g_console.size() - 16384);
}

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_hcode91_didjdroid_Native_init(JNIEnv* env, jclass, jstring jnand) {
    const char* p = env->GetStringUTFChars(jnand, nullptr);
    std::string path(p);
    env->ReleaseStringUTFChars(jnand, p);
    delete g_sys;
    g_sys = new Didj();
    g_sys->log_unknown = 0;
    g_sys->on_uart = [](char c) {
        unsigned char u = (unsigned char)c;
        if (u >= 0x80 || (u < 0x20 && c != '\n' && c != '\t')) return;
        std::string s(1, c);
        append_console(s);
    };
    g_sys->on_log = [](const std::string& s) {
        __android_log_print(ANDROID_LOG_INFO, "DidjDroid", "%s", s.c_str());
    };
    std::string err;
    if (!g_sys->load_nand(path, &err)) {
        delete g_sys; g_sys = nullptr;
        return env->NewStringUTF(err.c_str());
    }
    return env->NewStringUTF("");
}

JNIEXPORT void JNICALL
Java_com_hcode91_didjdroid_Native_runFrame(JNIEnv* env, jclass, jintArray pixels) {
    if (!g_sys) return;
    g_sys->run_frame();
    env->SetIntArrayRegion(pixels, 0, Didj::SCREEN_W * Didj::SCREEN_H, (const jint*)g_sys->framebuffer());
}

JNIEXPORT void JNICALL
Java_com_hcode91_didjdroid_Native_setButtons(JNIEnv*, jclass, jint mask) {
    if (g_sys) g_sys->set_buttons((u32)mask);
}

JNIEXPORT jint JNICALL
Java_com_hcode91_didjdroid_Native_readAudio(JNIEnv* env, jclass, jshortArray buf) {
    if (!g_sys) return 0;
    jsize len = env->GetArrayLength(buf);
    jshort* p = env->GetShortArrayElements(buf, nullptr);
    size_t n = g_sys->audio_read((int16_t*)p, (size_t)len / 2);
    env->ReleaseShortArrayElements(buf, p, 0);
    return (jint)n;
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_didjdroid_Native_saveNand(JNIEnv* env, jclass, jstring jpath) {
    if (!g_sys || !g_sys->nand_base.dirty) return JNI_FALSE;
    const char* p = env->GetStringUTFChars(jpath, nullptr);
    bool ok = g_sys->save_nand(p);
    env->ReleaseStringUTFChars(jpath, p);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_hcode91_didjdroid_Native_loadCart(JNIEnv* env, jclass, jstring jpath) {
    if (!g_sys) return env->NewStringUTF("emulator not started");
    const char* p = env->GetStringUTFChars(jpath, nullptr);
    std::string path(p);
    env->ReleaseStringUTFChars(jpath, p);
    std::string err;
    if (!g_sys->load_cart(path, &err)) return env->NewStringUTF(err.c_str());
    return env->NewStringUTF("");
}

JNIEXPORT void JNICALL
Java_com_hcode91_didjdroid_Native_ejectCart(JNIEnv*, jclass) {
    if (g_sys) g_sys->eject_cart();
}

JNIEXPORT void JNICALL
Java_com_hcode91_didjdroid_Native_reset(JNIEnv*, jclass) {
    if (g_sys) g_sys->reset();
}

JNIEXPORT jstring JNICALL
Java_com_hcode91_didjdroid_Native_console(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_log_mx);
    return env->NewStringUTF(g_console.c_str());
}

} // extern "C"
