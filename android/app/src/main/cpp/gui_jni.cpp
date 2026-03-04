#include <frametap/frametap.h>
#include <jni.h>

#include <exception>

extern "C" JNIEXPORT void JNICALL
Java_com_daccord_frametap_gui_FrametapGuiActivity_nativeInit(JNIEnv *env,
                                                             jobject activity) {
  try {
    frametap::android_init(env, activity);
  } catch (const std::exception &e) {
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls)
      env->ThrowNew(cls, e.what());
  }
}
