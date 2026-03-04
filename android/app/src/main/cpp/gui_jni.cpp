#include <frametap/frametap.h>
#include <jni.h>

extern "C" JNIEXPORT void JNICALL
Java_com_daccord_frametap_gui_FrametapGuiActivity_nativeInit(JNIEnv *env,
                                                             jobject activity) {
  frametap::android_init(env, activity);
}
