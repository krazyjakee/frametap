#include "decode/nvdec_decoder.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "encode/dynlib.h"

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_nvcuvid.h>

namespace {

constexpr CUresult CUDA_OK = CUDA_SUCCESS;

// CUDA driver symbols we need, resolved from libcuda.so.1 at runtime.
struct CudaApi {
  void *lib = nullptr;
  tcuInit *cuInit = nullptr;
  tcuDeviceGet *cuDeviceGet = nullptr;
  tcuCtxCreate_v2 *cuCtxCreate = nullptr;
  tcuCtxDestroy_v2 *cuCtxDestroy = nullptr;
  tcuCtxPushCurrent_v2 *cuCtxPushCurrent = nullptr;
  tcuCtxPopCurrent_v2 *cuCtxPopCurrent = nullptr;
  tcuMemcpy2D_v2 *cuMemcpy2D = nullptr;

  bool load() {
    lib = frametap::enc::dl_open(FT_CUDA_LIB);
    if (!lib)
      lib = frametap::enc::dl_open(FT_CUDA_LIB_ALT);
    if (!lib)
      return false;
    auto sym = [&](const char *n) { return frametap::enc::dl_sym(lib, n); };
    cuInit = reinterpret_cast<tcuInit *>(sym("cuInit"));
    cuDeviceGet = reinterpret_cast<tcuDeviceGet *>(sym("cuDeviceGet"));
    cuCtxCreate = reinterpret_cast<tcuCtxCreate_v2 *>(sym("cuCtxCreate_v2"));
    cuCtxDestroy = reinterpret_cast<tcuCtxDestroy_v2 *>(sym("cuCtxDestroy_v2"));
    cuCtxPushCurrent =
        reinterpret_cast<tcuCtxPushCurrent_v2 *>(sym("cuCtxPushCurrent_v2"));
    cuCtxPopCurrent =
        reinterpret_cast<tcuCtxPopCurrent_v2 *>(sym("cuCtxPopCurrent_v2"));
    cuMemcpy2D = reinterpret_cast<tcuMemcpy2D_v2 *>(sym("cuMemcpy2D_v2"));
    return cuInit && cuDeviceGet && cuCtxCreate && cuCtxDestroy &&
           cuCtxPushCurrent && cuCtxPopCurrent && cuMemcpy2D;
  }
};

// libnvcuvid.so.1 symbols.
struct CuvidApi {
  void *lib = nullptr;
  tcuvidCreateVideoParser *CreateVideoParser = nullptr;
  tcuvidParseVideoData *ParseVideoData = nullptr;
  tcuvidDestroyVideoParser *DestroyVideoParser = nullptr;
  tcuvidCreateDecoder *CreateDecoder = nullptr;
  tcuvidDestroyDecoder *DestroyDecoder = nullptr;
  tcuvidDecodePicture *DecodePicture = nullptr;
  tcuvidMapVideoFrame64 *MapVideoFrame = nullptr;
  tcuvidUnmapVideoFrame64 *UnmapVideoFrame = nullptr;
  tcuvidCtxLockCreate *CtxLockCreate = nullptr;
  tcuvidCtxLockDestroy *CtxLockDestroy = nullptr;

  bool load() {
    lib = frametap::enc::dl_open(FT_NVCUVID_LIB);
    if (!lib)
      lib = frametap::enc::dl_open(FT_NVCUVID_LIB_ALT);
    if (!lib)
      return false;
    auto sym = [&](const char *n) { return frametap::enc::dl_sym(lib, n); };
    CreateVideoParser =
        reinterpret_cast<tcuvidCreateVideoParser *>(sym("cuvidCreateVideoParser"));
    ParseVideoData =
        reinterpret_cast<tcuvidParseVideoData *>(sym("cuvidParseVideoData"));
    DestroyVideoParser = reinterpret_cast<tcuvidDestroyVideoParser *>(
        sym("cuvidDestroyVideoParser"));
    CreateDecoder =
        reinterpret_cast<tcuvidCreateDecoder *>(sym("cuvidCreateDecoder"));
    DestroyDecoder =
        reinterpret_cast<tcuvidDestroyDecoder *>(sym("cuvidDestroyDecoder"));
    DecodePicture =
        reinterpret_cast<tcuvidDecodePicture *>(sym("cuvidDecodePicture"));
    MapVideoFrame =
        reinterpret_cast<tcuvidMapVideoFrame64 *>(sym("cuvidMapVideoFrame64"));
    UnmapVideoFrame = reinterpret_cast<tcuvidUnmapVideoFrame64 *>(
        sym("cuvidUnmapVideoFrame64"));
    CtxLockCreate =
        reinterpret_cast<tcuvidCtxLockCreate *>(sym("cuvidCtxLockCreate"));
    CtxLockDestroy =
        reinterpret_cast<tcuvidCtxLockDestroy *>(sym("cuvidCtxLockDestroy"));
    return CreateVideoParser && ParseVideoData && DestroyVideoParser &&
           CreateDecoder && DestroyDecoder && DecodePicture && MapVideoFrame &&
           UnmapVideoFrame && CtxLockCreate && CtxLockDestroy;
  }
};

inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// NV12 (host, tightly packed: Y plane w*h, then interleaved UV w*(h/2)) to RGBA.
// Coefficients selected from the bitstream's signalled matrix/range.
void nv12_to_rgba(const uint8_t *nv12, int w, int h, bool full_range,
                  bool bt709, std::vector<uint8_t> &out) {
  out.resize(static_cast<size_t>(w) * h * 4);
  const uint8_t *yp = nv12;
  const uint8_t *uvp = nv12 + static_cast<size_t>(w) * h;

  // 8.8 fixed-point coefficients.
  int kr, kgu, kgv, kb, yscale, ysub;
  if (full_range) {
    yscale = 256;
    ysub = 0;
    if (bt709) {
      kr = 403;
      kgu = -48;
      kgv = -120;
      kb = 475;
    } else {
      kr = 359;
      kgu = -88;
      kgv = -183;
      kb = 454;
    }
  } else {
    yscale = 298; // 1.164 * 256
    ysub = 16;
    if (bt709) {
      kr = 459;
      kgu = -55;
      kgv = -136;
      kb = 541;
    } else {
      kr = 409;
      kgu = -100;
      kgv = -208;
      kb = 516;
    }
  }

  for (int y = 0; y < h; ++y) {
    const uint8_t *yrow = yp + static_cast<size_t>(y) * w;
    const uint8_t *uvrow = uvp + static_cast<size_t>(y / 2) * w;
    uint8_t *drow = out.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const int c = (yrow[x] - ysub) * yscale;
      const int d = uvrow[(x & ~1)] - 128;     // U
      const int e = uvrow[(x & ~1) + 1] - 128; // V
      drow[x * 4 + 0] = clamp8((c + kr * e + 128) >> 8);
      drow[x * 4 + 1] = clamp8((c + kgu * d + kgv * e + 128) >> 8);
      drow[x * 4 + 2] = clamp8((c + kb * d + 128) >> 8);
      drow[x * 4 + 3] = 0xFF;
    }
  }
}

} // namespace

namespace frametap::dec {

struct NvdecDecoder::Impl {
  CudaApi cuda;
  CuvidApi cuvid;
  CUcontext ctx = nullptr;
  CUvideoctxlock lock = nullptr;
  CUvideoparser parser = nullptr;
  CUvideodecoder decoder = nullptr;

  Codec codec = Codec::h264;
  FrameSink sink;
  bool opened = false;

  // Geometry of the live sequence.
  unsigned coded_width = 0, coded_height = 0;
  int out_w = 0, out_h = 0;
  int crop_left = 0, crop_top = 0;
  bool full_range = false;
  bool bt709 = true;

  std::vector<uint8_t> nv12_host; // staging buffer for the decoded surface
  std::vector<uint8_t> rgba;      // conversion output

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

  // --- Parser callbacks (static trampolines) ---
  static int CUDAAPI on_sequence(void *user, CUVIDEOFORMAT *fmt) {
    return static_cast<Impl *>(user)->handle_sequence(fmt);
  }
  static int CUDAAPI on_decode(void *user, CUVIDPICPARAMS *pic) {
    return static_cast<Impl *>(user)->handle_decode(pic);
  }
  static int CUDAAPI on_display(void *user, CUVIDPARSERDISPINFO *disp) {
    return static_cast<Impl *>(user)->handle_display(disp);
  }

  int handle_sequence(CUVIDEOFORMAT *fmt);
  int handle_decode(CUVIDPICPARAMS *pic);
  int handle_display(CUVIDPARSERDISPINFO *disp);

  void open(Codec c, FrameSink s);
  void decode(const uint8_t *annexb, size_t size, uint64_t pts);
  void flush();
  void close();
};

int NvdecDecoder::Impl::handle_sequence(CUVIDEOFORMAT *fmt) {
  int decode_surfaces = fmt->min_num_decode_surfaces;
  if (decode_surfaces < 1)
    decode_surfaces = 1;

  out_w = fmt->display_area.right - fmt->display_area.left;
  out_h = fmt->display_area.bottom - fmt->display_area.top;
  crop_left = fmt->display_area.left;
  crop_top = fmt->display_area.top;
  coded_width = fmt->coded_width;
  coded_height = fmt->coded_height;

  const int mc = fmt->video_signal_description.matrix_coefficients;
  bt709 = !(mc == 5 || mc == 6); // 5/6 = BT.601, else treat as 709
  full_range = fmt->video_signal_description.video_full_range_flag != 0;

  if (decoder) {
    cuvid.DestroyDecoder(decoder);
    decoder = nullptr;
  }

  CUVIDDECODECREATEINFO ci{};
  ci.CodecType = fmt->codec;
  ci.ChromaFormat = fmt->chroma_format;
  ci.OutputFormat = cudaVideoSurfaceFormat_NV12;
  ci.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
  ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
  ci.ulNumOutputSurfaces = 2;
  ci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
  ci.ulNumDecodeSurfaces = static_cast<tcu_ulong>(decode_surfaces);
  ci.vidLock = lock;
  ci.ulWidth = fmt->coded_width;
  ci.ulHeight = fmt->coded_height;
  ci.ulMaxWidth = fmt->coded_width;
  ci.ulMaxHeight = fmt->coded_height;
  ci.ulTargetWidth = fmt->coded_width;
  ci.ulTargetHeight = fmt->coded_height;

  if (cuvid.CreateDecoder(&decoder, &ci) != CUDA_OK) {
    decoder = nullptr;
    return 0; // abort parsing
  }
  return decode_surfaces;
}

int NvdecDecoder::Impl::handle_decode(CUVIDPICPARAMS *pic) {
  if (!decoder)
    return 0;
  return cuvid.DecodePicture(decoder, pic) == CUDA_OK ? 1 : 0;
}

int NvdecDecoder::Impl::handle_display(CUVIDPARSERDISPINFO *disp) {
  if (!decoder || !sink)
    return 1;

  CUVIDPROCPARAMS pp{};
  pp.progressive_frame = disp->progressive_frame;
  pp.top_field_first = disp->top_field_first;
  pp.unpaired_field = disp->repeat_first_field < 0;

  unsigned long long src = 0;
  unsigned int pitch = 0;
  if (cuvid.MapVideoFrame(decoder, disp->picture_index, &src, &pitch, &pp) !=
      CUDA_OK)
    return 0;

  const int chroma_h = (out_h + 1) / 2;
  nv12_host.resize(static_cast<size_t>(out_w) * out_h +
                   static_cast<size_t>(out_w) * chroma_h);

  // Luma plane (cropped to the display area).
  CUDA_MEMCPY2D m{};
  m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  m.srcDevice = src + static_cast<unsigned long long>(crop_top) * pitch +
                static_cast<unsigned long long>(crop_left);
  m.srcPitch = pitch;
  m.dstMemoryType = CU_MEMORYTYPE_HOST;
  m.dstHost = nv12_host.data();
  m.dstPitch = static_cast<size_t>(out_w);
  m.WidthInBytes = static_cast<size_t>(out_w);
  m.Height = static_cast<size_t>(out_h);
  CUresult r1 = cuda.cuMemcpy2D(&m);

  // Interleaved chroma plane (half height), located after the full luma
  // surface (coded_height rows) in device memory.
  m.srcDevice = src +
                static_cast<unsigned long long>(coded_height) * pitch +
                static_cast<unsigned long long>(crop_top / 2) * pitch +
                static_cast<unsigned long long>(crop_left);
  m.dstHost = nv12_host.data() + static_cast<size_t>(out_w) * out_h;
  m.Height = static_cast<size_t>(chroma_h);
  CUresult r2 = cuda.cuMemcpy2D(&m);

  cuvid.UnmapVideoFrame(decoder, src);

  if (r1 != CUDA_OK || r2 != CUDA_OK)
    return 0;

  nv12_to_rgba(nv12_host.data(), out_w, out_h, full_range, bt709, rgba);
  sink(rgba.data(), out_w, out_h,
       static_cast<uint64_t>(disp->timestamp));
  return 1;
}

void NvdecDecoder::Impl::open(Codec c, FrameSink s) {
  codec = c;
  sink = std::move(s);

  if (!cuda.load())
    throw std::runtime_error("NVDEC: failed to load " FT_CUDA_LIB
                             " (is the NVIDIA driver installed?)");
  if (!cuvid.load())
    throw std::runtime_error("NVDEC: failed to load " FT_NVCUVID_LIB
                             " (NVDEC unavailable)");

  if (cuda.cuInit(0) != CUDA_OK)
    throw std::runtime_error("NVDEC: cuInit failed");
  CUdevice dev = 0;
  if (cuda.cuDeviceGet(&dev, 0) != CUDA_OK)
    throw std::runtime_error("NVDEC: cuDeviceGet(0) failed");
  if (cuda.cuCtxCreate(&ctx, 0, dev) != CUDA_OK)
    throw std::runtime_error("NVDEC: cuCtxCreate failed");
  CUcontext popped = nullptr;
  cuda.cuCtxPopCurrent(&popped); // detach; CtxScope owns it from here

  if (cuvid.CtxLockCreate(&lock, ctx) != CUDA_OK)
    throw std::runtime_error("NVDEC: cuvidCtxLockCreate failed");

  CUVIDPARSERPARAMS pp{};
  pp.CodecType = codec == Codec::hevc ? cudaVideoCodec_HEVC : cudaVideoCodec_H264;
  pp.ulMaxNumDecodeSurfaces = 1; // overridden by the sequence callback
  pp.ulClockRate = 90000;        // timestamps pass through in 90 kHz units
  pp.ulMaxDisplayDelay = 0;      // zero reorder delay (stream has no B-frames)
  pp.pUserData = this;
  pp.pfnSequenceCallback = on_sequence;
  pp.pfnDecodePicture = on_decode;
  pp.pfnDisplayPicture = on_display;
  if (cuvid.CreateVideoParser(&parser, &pp) != CUDA_OK)
    throw std::runtime_error("NVDEC: cuvidCreateVideoParser failed");

  opened = true;
}

void NvdecDecoder::Impl::decode(const uint8_t *annexb, size_t size,
                                uint64_t pts) {
  if (!opened)
    return;
  CtxScope scope(this);
  CUVIDSOURCEDATAPACKET pkt{};
  pkt.flags = CUVID_PKT_TIMESTAMP;
  pkt.payload_size = size;
  pkt.payload = annexb;
  pkt.timestamp = static_cast<CUvideotimestamp>(pts);
  cuvid.ParseVideoData(parser, &pkt);
}

void NvdecDecoder::Impl::flush() {
  if (!opened)
    return;
  CtxScope scope(this);
  CUVIDSOURCEDATAPACKET pkt{};
  pkt.flags = CUVID_PKT_ENDOFSTREAM;
  cuvid.ParseVideoData(parser, &pkt);
}

void NvdecDecoder::Impl::close() {
  if (parser) {
    cuvid.DestroyVideoParser(parser);
    parser = nullptr;
  }
  if (decoder) {
    cuvid.DestroyDecoder(decoder);
    decoder = nullptr;
  }
  if (lock) {
    cuvid.CtxLockDestroy(lock);
    lock = nullptr;
  }
  if (ctx) {
    cuda.cuCtxDestroy(ctx);
    ctx = nullptr;
  }
  if (cuvid.lib) {
    frametap::enc::dl_close(cuvid.lib);
    cuvid.lib = nullptr;
  }
  if (cuda.lib) {
    frametap::enc::dl_close(cuda.lib);
    cuda.lib = nullptr;
  }
  opened = false;
}

// --- Public shell ---

NvdecDecoder::NvdecDecoder() : impl_(new Impl()) {}
NvdecDecoder::~NvdecDecoder() {
  if (impl_) {
    impl_->close();
    delete impl_;
  }
}
void NvdecDecoder::open(Codec codec, FrameSink sink) {
  impl_->open(codec, std::move(sink));
}
void NvdecDecoder::decode(const uint8_t *annexb, size_t size, uint64_t pts) {
  impl_->decode(annexb, size, pts);
}
void NvdecDecoder::flush() { impl_->flush(); }
void NvdecDecoder::close() { impl_->close(); }
bool NvdecDecoder::is_open() const { return impl_ && impl_->opened; }

} // namespace frametap::dec
