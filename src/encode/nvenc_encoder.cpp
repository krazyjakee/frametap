#include "encode/nvenc_encoder.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <dlfcn.h>

#include <ffnvcodec/nvEncodeAPI.h>

// ---------------------------------------------------------------------------
// Minimal CUDA driver API surface.
//
// We resolve only the handful of driver symbols we need from libcuda.so.1 at
// runtime, with our own signatures, so the build needs neither cuda.h nor a
// link against libcuda. All exported names below are ABI-stable.
// ---------------------------------------------------------------------------
namespace {

using CUresult = int;
using CUdevice = int;
using CUcontext = void *;
using CUdeviceptr = unsigned long long;
constexpr CUresult CUDA_SUCCESS_ = 0;

using PFN_cuInit = CUresult (*)(unsigned int);
using PFN_cuDeviceGet = CUresult (*)(CUdevice *, int);
using PFN_cuCtxCreate = CUresult (*)(CUcontext *, unsigned int, CUdevice);
using PFN_cuCtxDestroy = CUresult (*)(CUcontext);
using PFN_cuCtxPushCurrent = CUresult (*)(CUcontext);
using PFN_cuCtxPopCurrent = CUresult (*)(CUcontext *);
using PFN_cuMemAlloc = CUresult (*)(CUdeviceptr *, size_t);
using PFN_cuMemFree = CUresult (*)(CUdeviceptr);
using PFN_cuMemcpyHtoD = CUresult (*)(CUdeviceptr, const void *, size_t);

struct CudaApi {
  void *lib = nullptr;
  PFN_cuInit cuInit = nullptr;
  PFN_cuDeviceGet cuDeviceGet = nullptr;
  PFN_cuCtxCreate cuCtxCreate = nullptr;
  PFN_cuCtxDestroy cuCtxDestroy = nullptr;
  PFN_cuCtxPushCurrent cuCtxPushCurrent = nullptr;
  PFN_cuCtxPopCurrent cuCtxPopCurrent = nullptr;
  PFN_cuMemAlloc cuMemAlloc = nullptr;
  PFN_cuMemFree cuMemFree = nullptr;
  PFN_cuMemcpyHtoD cuMemcpyHtoD = nullptr;

  bool load() {
    lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
      lib = dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
      return false;
    auto sym = [&](const char *n) { return dlsym(lib, n); };
    cuInit = reinterpret_cast<PFN_cuInit>(sym("cuInit"));
    cuDeviceGet = reinterpret_cast<PFN_cuDeviceGet>(sym("cuDeviceGet"));
    cuCtxCreate = reinterpret_cast<PFN_cuCtxCreate>(sym("cuCtxCreate_v2"));
    cuCtxDestroy = reinterpret_cast<PFN_cuCtxDestroy>(sym("cuCtxDestroy_v2"));
    cuCtxPushCurrent =
        reinterpret_cast<PFN_cuCtxPushCurrent>(sym("cuCtxPushCurrent_v2"));
    cuCtxPopCurrent =
        reinterpret_cast<PFN_cuCtxPopCurrent>(sym("cuCtxPopCurrent_v2"));
    cuMemAlloc = reinterpret_cast<PFN_cuMemAlloc>(sym("cuMemAlloc_v2"));
    cuMemFree = reinterpret_cast<PFN_cuMemFree>(sym("cuMemFree_v2"));
    cuMemcpyHtoD = reinterpret_cast<PFN_cuMemcpyHtoD>(sym("cuMemcpyHtoD_v2"));
    return cuInit && cuDeviceGet && cuCtxCreate && cuCtxDestroy &&
           cuCtxPushCurrent && cuCtxPopCurrent && cuMemAlloc && cuMemFree &&
           cuMemcpyHtoD;
  }
};

const char *nvenc_status_str(NVENCSTATUS s) {
  switch (s) {
  case NV_ENC_SUCCESS:
    return "SUCCESS";
  case NV_ENC_ERR_NO_ENCODE_DEVICE:
    return "NO_ENCODE_DEVICE";
  case NV_ENC_ERR_UNSUPPORTED_DEVICE:
    return "UNSUPPORTED_DEVICE";
  case NV_ENC_ERR_INVALID_ENCODERDEVICE:
    return "INVALID_ENCODERDEVICE";
  case NV_ENC_ERR_INVALID_PARAM:
    return "INVALID_PARAM";
  case NV_ENC_ERR_OUT_OF_MEMORY:
    return "OUT_OF_MEMORY";
  case NV_ENC_ERR_INVALID_VERSION:
    return "INVALID_VERSION";
  case NV_ENC_ERR_UNIMPLEMENTED:
    return "UNIMPLEMENTED";
  default:
    return "NVENC_ERROR";
  }
}

} // namespace

namespace frametap::enc {

struct NvencEncoder::Impl {
  CudaApi cuda;
  CUcontext ctx = nullptr;
  CUdeviceptr dev_frame = 0;
  size_t dev_frame_size = 0;

  void *nvenc_lib = nullptr;
  NV_ENCODE_API_FUNCTION_LIST fl{};
  void *encoder = nullptr;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_OUTPUT_PTR out_buf = nullptr;

  NV_ENC_INITIALIZE_PARAMS init{};
  NV_ENC_CONFIG config{};
  NvencParams params{};
  PacketSink sink;
  bool opened = false;

  // Push our CUDA context for the current thread; RAII pops it on scope exit
  // so encode()/flush()/close() are safe to call from any thread.
  struct CtxScope {
    Impl *o;
    explicit CtxScope(Impl *i) : o(i) {
      if (o->ctx)
        o->cuda.cuCtxPushCurrent(o->ctx);
    }
    ~CtxScope() {
      if (o->ctx) {
        CUcontext popped = nullptr;
        o->cuda.cuCtxPopCurrent(&popped);
      }
    }
  };

  [[noreturn]] void fail(const std::string &what, NVENCSTATUS s) {
    std::string msg = "NVENC: " + what + " (" + nvenc_status_str(s) + ")";
    if (encoder && fl.nvEncGetLastErrorString) {
      const char *detail = fl.nvEncGetLastErrorString(encoder);
      if (detail && *detail)
        msg += ": " + std::string(detail);
    }
    throw std::runtime_error(msg);
  }

  void check(NVENCSTATUS s, const char *what) {
    if (s != NV_ENC_SUCCESS)
      fail(what, s);
  }

  void open(const NvencParams &p, PacketSink s);
  void encode(const uint8_t *rgba, int w, int h, size_t stride, int64_t ts);
  void drain_output();
  void set_bitrate(int kbps);
  void apply_rate_control(int kbps);
  void flush();
  void close();
};

void NvencEncoder::Impl::apply_rate_control(int kbps) {
  uint32_t bps = static_cast<uint32_t>(kbps) * 1000u;
  int fps = params.fps > 0 ? params.fps : 60;
  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = bps;
  config.rcParams.maxBitRate = bps;
  // ~1-frame VBV keeps latency low (game-streaming oriented).
  config.rcParams.vbvBufferSize = bps / static_cast<uint32_t>(fps);
  config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
}

void NvencEncoder::Impl::open(const NvencParams &p, PacketSink s) {
  params = p;
  sink = std::move(s);

  if (!cuda.load())
    throw std::runtime_error(
        "NVENC: failed to load libcuda.so.1 (is the NVIDIA driver installed?)");

  nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!nvenc_lib)
    nvenc_lib = dlopen("libnvidia-encode.so", RTLD_NOW | RTLD_GLOBAL);
  if (!nvenc_lib)
    throw std::runtime_error(
        "NVENC: failed to load libnvidia-encode.so.1 (NVENC unavailable)");

  using PFN_Create = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST *);
  auto create = reinterpret_cast<PFN_Create>(
      dlsym(nvenc_lib, "NvEncodeAPICreateInstance"));
  if (!create)
    throw std::runtime_error("NVENC: NvEncodeAPICreateInstance not found");

  if (cuda.cuInit(0) != CUDA_SUCCESS_)
    throw std::runtime_error("NVENC: cuInit failed");
  CUdevice dev = 0;
  if (cuda.cuDeviceGet(&dev, 0) != CUDA_SUCCESS_)
    throw std::runtime_error("NVENC: cuDeviceGet(0) failed");
  if (cuda.cuCtxCreate(&ctx, 0, dev) != CUDA_SUCCESS_)
    throw std::runtime_error("NVENC: cuCtxCreate failed");
  // cuCtxCreate leaves the context current; detach so our CtxScope owns it.
  CUcontext popped = nullptr;
  cuda.cuCtxPopCurrent(&popped);

  CtxScope scope(this);

  fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  check(create(&fl), "NvEncodeAPICreateInstance");

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
  sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  sp.device = ctx;
  sp.apiVersion = NVENCAPI_VERSION;
  check(fl.nvEncOpenEncodeSessionEx(&sp, &encoder), "OpenEncodeSessionEx");

  const GUID codec_guid = params.codec == Codec::hevc
                              ? NV_ENC_CODEC_HEVC_GUID
                              : NV_ENC_CODEC_H264_GUID;
  const GUID preset_guid = NV_ENC_PRESET_P4_GUID;
  const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_LOW_LATENCY;

  NV_ENC_PRESET_CONFIG pc{};
  pc.version = NV_ENC_PRESET_CONFIG_VER;
  pc.presetCfg.version = NV_ENC_CONFIG_VER;
  check(fl.nvEncGetEncodePresetConfigEx(encoder, codec_guid, preset_guid,
                                        tuning, &pc),
        "GetEncodePresetConfigEx");

  config = pc.presetCfg;
  config.version = NV_ENC_CONFIG_VER;

  int fps = params.fps > 0 ? params.fps : 60;
  config.gopLength = static_cast<uint32_t>(fps) * 2u; // 2s GOP
  // No B-frames + low-latency tuning (no lookahead) means exactly one output
  // packet per input frame, which is what lets us use a single bitstream
  // buffer and drain it right after each encode(). If B-frames or lookahead
  // are ever enabled, encode() must switch to a ring of bitstream buffers.
  config.frameIntervalP = 1;
  apply_rate_control(params.bitrate_kbps);

  // Repeat SPS/PPS on every IDR so the elementary stream is self-contained
  // and directly playable (mpv/ffplay) without a container.
  if (params.codec == Codec::hevc) {
    config.encodeCodecConfig.hevcConfig.idrPeriod = config.gopLength;
    config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
  } else {
    config.encodeCodecConfig.h264Config.idrPeriod = config.gopLength;
    config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
  }

  init = NV_ENC_INITIALIZE_PARAMS{};
  init.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init.encodeGUID = codec_guid;
  init.presetGUID = preset_guid;
  init.encodeWidth = static_cast<uint32_t>(params.width);
  init.encodeHeight = static_cast<uint32_t>(params.height);
  init.darWidth = init.encodeWidth;
  init.darHeight = init.encodeHeight;
  init.maxEncodeWidth = init.encodeWidth;
  init.maxEncodeHeight = init.encodeHeight;
  init.frameRateNum = static_cast<uint32_t>(fps);
  init.frameRateDen = 1;
  init.enablePTD = 1;
  init.tuningInfo = tuning;
  init.encodeConfig = &config;
  check(fl.nvEncInitializeEncoder(encoder, &init), "InitializeEncoder");

  // Device-side staging frame: packed RGBA, uploaded per frame via cuMemcpyHtoD.
  dev_frame_size =
      static_cast<size_t>(params.width) * params.height * 4u;
  if (cuda.cuMemAlloc(&dev_frame, dev_frame_size) != CUDA_SUCCESS_)
    throw std::runtime_error("NVENC: cuMemAlloc for input frame failed");

  NV_ENC_REGISTER_RESOURCE rr{};
  rr.version = NV_ENC_REGISTER_RESOURCE_VER;
  rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
  rr.width = static_cast<uint32_t>(params.width);
  rr.height = static_cast<uint32_t>(params.height);
  rr.pitch = static_cast<uint32_t>(params.width) * 4u;
  rr.resourceToRegister = reinterpret_cast<void *>(dev_frame);
  rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; // RGBA byte order
  rr.bufferUsage = NV_ENC_INPUT_IMAGE;
  check(fl.nvEncRegisterResource(encoder, &rr), "RegisterResource");
  registered = rr.registeredResource;

  NV_ENC_CREATE_BITSTREAM_BUFFER cbb{};
  cbb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  check(fl.nvEncCreateBitstreamBuffer(encoder, &cbb), "CreateBitstreamBuffer");
  out_buf = cbb.bitstreamBuffer;

  opened = true;
}

void NvencEncoder::Impl::encode(const uint8_t *rgba, int w, int h,
                                size_t stride, int64_t ts) {
  if (!opened)
    return;
  CtxScope scope(this);

  const size_t row = static_cast<size_t>(w) * 4u;
  if (stride == row) {
    if (cuda.cuMemcpyHtoD(dev_frame, rgba, row * static_cast<size_t>(h)) !=
        CUDA_SUCCESS_)
      throw std::runtime_error("NVENC: cuMemcpyHtoD (frame) failed");
  } else {
    for (int y = 0; y < h; ++y) {
      if (cuda.cuMemcpyHtoD(dev_frame + static_cast<CUdeviceptr>(y) * row,
                            rgba + static_cast<size_t>(y) * stride, row) !=
          CUDA_SUCCESS_)
        throw std::runtime_error("NVENC: cuMemcpyHtoD (row) failed");
    }
  }

  NV_ENC_MAP_INPUT_RESOURCE mp{};
  mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  mp.registeredResource = registered;
  check(fl.nvEncMapInputResource(encoder, &mp), "MapInputResource");

  NV_ENC_PIC_PARAMS pp{};
  pp.version = NV_ENC_PIC_PARAMS_VER;
  pp.inputWidth = static_cast<uint32_t>(w);
  pp.inputHeight = static_cast<uint32_t>(h);
  pp.inputPitch = static_cast<uint32_t>(w) * 4u;
  pp.inputBuffer = mp.mappedResource;
  pp.bufferFmt = mp.mappedBufferFmt;
  pp.outputBitstream = out_buf;
  pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pp.inputTimeStamp = static_cast<uint64_t>(ts);

  NVENCSTATUS s = fl.nvEncEncodePicture(encoder, &pp);
  // Unmap regardless of encode result.
  fl.nvEncUnmapInputResource(encoder, mp.mappedResource);

  if (s == NV_ENC_SUCCESS)
    drain_output();
  else if (s != NV_ENC_ERR_NEED_MORE_INPUT)
    fail("EncodePicture", s);
}

void NvencEncoder::Impl::drain_output() {
  NV_ENC_LOCK_BITSTREAM lb{};
  lb.version = NV_ENC_LOCK_BITSTREAM_VER;
  lb.outputBitstream = out_buf;
  check(fl.nvEncLockBitstream(encoder, &lb), "LockBitstream");

  const bool keyframe = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
  if (sink)
    sink(static_cast<const uint8_t *>(lb.bitstreamBufferPtr),
         lb.bitstreamSizeInBytes, keyframe);

  fl.nvEncUnlockBitstream(encoder, out_buf);
}

void NvencEncoder::Impl::set_bitrate(int kbps) {
  if (!opened)
    return;
  CtxScope scope(this);
  apply_rate_control(kbps);

  NV_ENC_RECONFIGURE_PARAMS rp{};
  rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  rp.reInitEncodeParams = init; // encodeConfig still points at our member
  check(fl.nvEncReconfigureEncoder(encoder, &rp), "ReconfigureEncoder");
}

void NvencEncoder::Impl::flush() {
  if (!opened)
    return;
  CtxScope scope(this);
  NV_ENC_PIC_PARAMS pp{};
  pp.version = NV_ENC_PIC_PARAMS_VER;
  pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
  fl.nvEncEncodePicture(encoder, &pp);
  // No B-frames / sync mode -> nothing buffered to drain after EOS.
}

void NvencEncoder::Impl::close() {
  if (ctx)
    cuda.cuCtxPushCurrent(ctx);

  if (encoder) {
    if (registered) {
      fl.nvEncUnregisterResource(encoder, registered);
      registered = nullptr;
    }
    if (out_buf) {
      fl.nvEncDestroyBitstreamBuffer(encoder, out_buf);
      out_buf = nullptr;
    }
    fl.nvEncDestroyEncoder(encoder);
    encoder = nullptr;
  }
  if (dev_frame) {
    cuda.cuMemFree(dev_frame);
    dev_frame = 0;
  }

  if (ctx) {
    CUcontext popped = nullptr;
    cuda.cuCtxPopCurrent(&popped);
    cuda.cuCtxDestroy(ctx);
    ctx = nullptr;
  }
  if (nvenc_lib) {
    dlclose(nvenc_lib);
    nvenc_lib = nullptr;
  }
  if (cuda.lib) {
    dlclose(cuda.lib);
    cuda.lib = nullptr;
  }
  opened = false;
}

// --- Public shell ---------------------------------------------------------

NvencEncoder::NvencEncoder() : impl_(new Impl()) {}
NvencEncoder::~NvencEncoder() {
  if (impl_) {
    impl_->close();
    delete impl_;
  }
}

void NvencEncoder::open(const NvencParams &params, PacketSink sink) {
  impl_->open(params, std::move(sink));
}
void NvencEncoder::encode(const uint8_t *rgba, int width, int height,
                          size_t stride, int64_t timestamp) {
  impl_->encode(rgba, width, height, stride, timestamp);
}
void NvencEncoder::set_bitrate(int bitrate_kbps) {
  impl_->set_bitrate(bitrate_kbps);
}
void NvencEncoder::flush() { impl_->flush(); }
void NvencEncoder::close() { impl_->close(); }
bool NvencEncoder::is_open() const { return impl_ && impl_->opened; }

} // namespace frametap::enc
