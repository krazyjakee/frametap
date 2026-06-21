#pragma once

// Cross-platform dynamic library loading for the runtime-resolved NVIDIA driver
// symbols (CUDA driver API + NVENC). The encoder links neither libcuda nor
// libnvidia-encode at build time -- it resolves them at open() time -- so this
// shim keeps the dlfcn (POSIX) / LoadLibrary (Win32) #ifdef out of
// nvenc_encoder.cpp and lets the same code drive both platforms.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace frametap::enc {
inline void *dl_open(const char *name) {
  return reinterpret_cast<void *>(::LoadLibraryA(name));
}
inline void *dl_sym(void *lib, const char *name) {
  return reinterpret_cast<void *>(
      ::GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
}
inline void dl_close(void *lib) {
  if (lib)
    ::FreeLibrary(reinterpret_cast<HMODULE>(lib));
}
} // namespace frametap::enc

// Primary and fallback module names for the NVIDIA driver libraries.
#define FT_CUDA_LIB "nvcuda.dll"
#define FT_CUDA_LIB_ALT "nvcuda.dll"
#define FT_NVENC_LIB "nvEncodeAPI64.dll"
#define FT_NVENC_LIB_ALT "nvEncodeAPI.dll"
#define FT_NVCUVID_LIB "nvcuvid.dll"
#define FT_NVCUVID_LIB_ALT "nvcuvid.dll"

#else // POSIX
#include <dlfcn.h>

namespace frametap::enc {
inline void *dl_open(const char *name) {
  return ::dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}
inline void *dl_sym(void *lib, const char *name) { return ::dlsym(lib, name); }
inline void dl_close(void *lib) {
  if (lib)
    ::dlclose(lib);
}
} // namespace frametap::enc

#define FT_CUDA_LIB "libcuda.so.1"
#define FT_CUDA_LIB_ALT "libcuda.so"
#define FT_NVENC_LIB "libnvidia-encode.so.1"
#define FT_NVENC_LIB_ALT "libnvidia-encode.so"
#define FT_NVCUVID_LIB "libnvcuvid.so.1"
#define FT_NVCUVID_LIB_ALT "libnvcuvid.so"
#endif
