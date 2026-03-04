#include "jni_bridge.h"

#include <frametap/frametap.h>

#include <jni.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

namespace frametap::internal::jni {

// ---------------------------------------------------------------------------
// Global JNI state
// ---------------------------------------------------------------------------

static JavaVM *g_jvm = nullptr;
static jobject g_activity = nullptr;    // Global ref
static jclass g_projection_cls = nullptr; // Global ref

static bool g_initialized = false;

// Consent synchronization
static std::mutex g_consent_mutex;
static std::condition_variable g_consent_cv;
static bool g_consent_ready = false;
static bool g_consent_granted = false;

// ---------------------------------------------------------------------------
// JniScope — RAII helper for attaching/detaching JNI on arbitrary threads
// ---------------------------------------------------------------------------

class JniScope {
public:
  JniScope() {
    if (!g_jvm)
      throw CaptureError("JNI not initialized");

    JNIEnv *env = nullptr;
    jint status = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
      env_ = env;
      needs_detach_ = false;
    } else if (status == JNI_EDETACHED) {
      JavaVMAttachArgs args{JNI_VERSION_1_6, nullptr, nullptr};
      if (g_jvm->AttachCurrentThread(&env_, &args) != JNI_OK)
        throw CaptureError("Failed to attach JNI thread");
      needs_detach_ = true;
    } else {
      throw CaptureError("Failed to get JNI environment");
    }
  }

  ~JniScope() {
    if (needs_detach_ && g_jvm)
      g_jvm->DetachCurrentThread();
  }

  JniScope(const JniScope &) = delete;
  JniScope &operator=(const JniScope &) = delete;

  JNIEnv *env() const { return env_; }

private:
  JNIEnv *env_ = nullptr;
  bool needs_detach_ = false;
};

// ---------------------------------------------------------------------------
// JNI exception checking
// ---------------------------------------------------------------------------

static void check_jni_exception(JNIEnv *env, const char *context) {
  if (env->ExceptionCheck()) {
    jthrowable exc = env->ExceptionOccurred();
    env->ExceptionClear();

    std::string msg = "JNI exception in ";
    msg += context;

    // Try to get the exception message
    if (exc) {
      jclass throwable_cls = env->FindClass("java/lang/Throwable");
      if (throwable_cls) {
        jmethodID get_message =
            env->GetMethodID(throwable_cls, "getMessage", "()Ljava/lang/String;");
        if (get_message) {
          auto jmsg =
              static_cast<jstring>(env->CallObjectMethod(exc, get_message));
          if (jmsg) {
            const char *chars = env->GetStringUTFChars(jmsg, nullptr);
            if (chars) {
              msg += ": ";
              msg += chars;
              env->ReleaseStringUTFChars(jmsg, chars);
            }
            env->DeleteLocalRef(jmsg);
          }
        }
        env->DeleteLocalRef(throwable_cls);
      }
      env->DeleteLocalRef(exc);
    }

    throw CaptureError(msg);
  }
}

// ---------------------------------------------------------------------------
// Native callback from Java (consent result)
// ---------------------------------------------------------------------------

static void JNICALL native_on_consent_result(JNIEnv * /*env*/, jclass /*cls*/,
                                              jboolean granted) {
  std::lock_guard lock(g_consent_mutex);
  g_consent_granted = (granted == JNI_TRUE);
  g_consent_ready = true;
  g_consent_cv.notify_all();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void jni_init(JNIEnv *env, jobject activity) {
  if (g_initialized)
    return;

  // Cache the JVM
  if (env->GetJavaVM(&g_jvm) != JNI_OK)
    throw CaptureError("Failed to get JavaVM from JNIEnv");

  // Store a global reference to the activity
  g_activity = env->NewGlobalRef(activity);
  if (!g_activity)
    throw CaptureError("Failed to create global reference to Activity");

  // Find and cache the FrametapProjection class
  jclass local_cls = env->FindClass("com/daccord/frametap/FrametapProjection");
  check_jni_exception(env, "FindClass FrametapProjection");
  if (!local_cls)
    throw CaptureError("Cannot find com.daccord.frametap.FrametapProjection class");

  g_projection_cls = static_cast<jclass>(env->NewGlobalRef(local_cls));
  env->DeleteLocalRef(local_cls);

  // Register native methods
  JNINativeMethod methods[] = {
      {const_cast<char *>("nativeOnConsentResult"), const_cast<char *>("(Z)V"),
       reinterpret_cast<void *>(native_on_consent_result)},
  };
  if (env->RegisterNatives(g_projection_cls, methods, 1) < 0) {
    check_jni_exception(env, "RegisterNatives");
    throw CaptureError("Failed to register native methods on FrametapProjection");
  }

  // Call FrametapProjection.getInstance().init(activity)
  jmethodID get_instance = env->GetStaticMethodID(
      g_projection_cls, "getInstance",
      "()Lcom/daccord/frametap/FrametapProjection;");
  check_jni_exception(env, "GetStaticMethodID getInstance");

  jobject instance = env->CallStaticObjectMethod(g_projection_cls, get_instance);
  check_jni_exception(env, "CallStaticObjectMethod getInstance");

  jmethodID init_method = env->GetMethodID(g_projection_cls, "init",
                                           "(Landroid/app/Activity;)V");
  check_jni_exception(env, "GetMethodID init");

  env->CallVoidMethod(instance, init_method, activity);
  check_jni_exception(env, "CallVoidMethod init");

  env->DeleteLocalRef(instance);

  g_initialized = true;
}

bool jni_is_initialized() { return g_initialized; }

bool jni_request_consent() {
  if (!g_initialized)
    throw CaptureError("JNI not initialized — call android_init() first");

  // Reset consent state
  {
    std::lock_guard lock(g_consent_mutex);
    g_consent_ready = false;
    g_consent_granted = false;
  }

  // Call requestConsent() on the Java side
  {
    JniScope scope;
    JNIEnv *env = scope.env();

    jmethodID get_instance = env->GetStaticMethodID(
        g_projection_cls, "getInstance",
        "()Lcom/daccord/frametap/FrametapProjection;");
    check_jni_exception(env, "GetStaticMethodID getInstance");

    jobject instance =
        env->CallStaticObjectMethod(g_projection_cls, get_instance);
    check_jni_exception(env, "getInstance");

    jmethodID request = env->GetMethodID(g_projection_cls, "requestConsent", "()V");
    check_jni_exception(env, "GetMethodID requestConsent");

    env->CallVoidMethod(instance, request);
    check_jni_exception(env, "requestConsent");

    env->DeleteLocalRef(instance);
  }

  // Block until the Java callback fires
  std::unique_lock lock(g_consent_mutex);
  g_consent_cv.wait(lock, [] { return g_consent_ready; });

  return g_consent_granted;
}

bool jni_is_projection_active() {
  if (!g_initialized)
    return false;

  JniScope scope;
  JNIEnv *env = scope.env();

  jmethodID get_instance = env->GetStaticMethodID(
      g_projection_cls, "getInstance",
      "()Lcom/daccord/frametap/FrametapProjection;");
  jobject instance =
      env->CallStaticObjectMethod(g_projection_cls, get_instance);
  check_jni_exception(env, "getInstance");

  jmethodID is_active =
      env->GetMethodID(g_projection_cls, "isActive", "()Z");
  check_jni_exception(env, "GetMethodID isActive");

  jboolean active = env->CallBooleanMethod(instance, is_active);
  check_jni_exception(env, "isActive");

  env->DeleteLocalRef(instance);
  return active == JNI_TRUE;
}

ImageData jni_capture_frame() {
  if (!g_initialized)
    throw CaptureError("JNI not initialized — call android_init() first");

  JniScope scope;
  JNIEnv *env = scope.env();

  jmethodID get_instance = env->GetStaticMethodID(
      g_projection_cls, "getInstance",
      "()Lcom/daccord/frametap/FrametapProjection;");
  jobject instance =
      env->CallStaticObjectMethod(g_projection_cls, get_instance);
  check_jni_exception(env, "getInstance");

  jmethodID capture =
      env->GetMethodID(g_projection_cls, "captureFrame", "()[B");
  check_jni_exception(env, "GetMethodID captureFrame");

  auto byte_array =
      static_cast<jbyteArray>(env->CallObjectMethod(instance, capture));
  check_jni_exception(env, "captureFrame");

  if (!byte_array) {
    env->DeleteLocalRef(instance);
    // No frame available yet — caller should retry
    return {};
  }

  jsize len = env->GetArrayLength(byte_array);

  // Get display dimensions to compute width/height
  jmethodID get_w =
      env->GetMethodID(g_projection_cls, "getDisplayWidth", "()I");
  jmethodID get_h =
      env->GetMethodID(g_projection_cls, "getDisplayHeight", "()I");
  check_jni_exception(env, "GetMethodID display size");

  int width = env->CallIntMethod(instance, get_w);
  int height = env->CallIntMethod(instance, get_h);
  check_jni_exception(env, "getDisplayWidth/Height");

  ImageData result;
  result.width = static_cast<size_t>(width);
  result.height = static_cast<size_t>(height);
  result.data.resize(static_cast<size_t>(len));

  env->GetByteArrayRegion(byte_array, 0, len,
                          reinterpret_cast<jbyte *>(result.data.data()));
  check_jni_exception(env, "GetByteArrayRegion");

  env->DeleteLocalRef(byte_array);
  env->DeleteLocalRef(instance);

  return result;
}

std::pair<int, int> jni_get_display_size() {
  if (!g_initialized)
    return {0, 0};

  JniScope scope;
  JNIEnv *env = scope.env();

  jmethodID get_instance = env->GetStaticMethodID(
      g_projection_cls, "getInstance",
      "()Lcom/daccord/frametap/FrametapProjection;");
  jobject instance =
      env->CallStaticObjectMethod(g_projection_cls, get_instance);
  check_jni_exception(env, "getInstance");

  jmethodID get_w =
      env->GetMethodID(g_projection_cls, "getDisplayWidth", "()I");
  jmethodID get_h =
      env->GetMethodID(g_projection_cls, "getDisplayHeight", "()I");
  check_jni_exception(env, "GetMethodID display size");

  int w = env->CallIntMethod(instance, get_w);
  int h = env->CallIntMethod(instance, get_h);
  check_jni_exception(env, "getDisplayWidth/Height");

  env->DeleteLocalRef(instance);
  return {w, h};
}

void jni_stop_projection() {
  if (!g_initialized)
    return;

  JniScope scope;
  JNIEnv *env = scope.env();

  jmethodID get_instance = env->GetStaticMethodID(
      g_projection_cls, "getInstance",
      "()Lcom/daccord/frametap/FrametapProjection;");
  jobject instance =
      env->CallStaticObjectMethod(g_projection_cls, get_instance);
  check_jni_exception(env, "getInstance");

  jmethodID stop_method =
      env->GetMethodID(g_projection_cls, "stopProjection", "()V");
  check_jni_exception(env, "GetMethodID stopProjection");

  env->CallVoidMethod(instance, stop_method);
  check_jni_exception(env, "stopProjection");

  env->DeleteLocalRef(instance);
}

} // namespace frametap::internal::jni

// ---------------------------------------------------------------------------
// Extern "C" JNI entry point (fallback for System.loadLibrary path)
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_com_daccord_frametap_FrametapProjection_nativeOnConsentResult(
    JNIEnv *env, jclass cls, jboolean granted) {
  frametap::internal::jni::native_on_consent_result(env, cls, granted);
}
