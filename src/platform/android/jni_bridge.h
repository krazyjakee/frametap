#pragma once

#include <frametap/types.h>

#include <jni.h>
#include <utility>

namespace frametap::internal::jni {

// Initialize the JNI bridge. Must be called from a thread with a valid JNIEnv
// (typically the main/UI thread). Stores a global reference to the JVM and
// Activity, caches the FrametapProjection class, and registers native methods.
void jni_init(JNIEnv *env, jobject activity);

// Returns true if jni_init() has been called successfully.
bool jni_is_initialized();

// Launch the consent Activity and block until the user grants or denies.
// Returns true if consent was granted.
// Throws CaptureError if JNI is not initialized or if Java throws.
bool jni_request_consent();

// Returns true if MediaProjection is active and capturing.
bool jni_is_projection_active();

// Capture a single frame via MediaProjection/ImageReader.
// Returns an ImageData with RGBA pixels.
// Throws CaptureError on failure.
ImageData jni_capture_frame();

// Query the device display dimensions.
// Returns {width, height}.
std::pair<int, int> jni_get_display_size();

// Stop the MediaProjection, VirtualDisplay, and foreground service.
void jni_stop_projection();

} // namespace frametap::internal::jni
