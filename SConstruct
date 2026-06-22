import sys
import os

# --- Platform / target detection ---
# Usage: scons target=android   (cross-compile for Android via NDK)
# By default, target matches the host platform.
platform = sys.platform  # 'darwin', 'linux', 'win32'
target = ARGUMENTS.get('target', platform)

# --- Sanitizer option ---
# Usage: scons sanitize=address
#        scons sanitize=thread
#        scons sanitize=undefined
sanitize = ARGUMENTS.get('sanitize', 'none')

# --- macOS architecture option ---
# Usage: scons macos_arch=arm64      (Apple Silicon only)
#        scons macos_arch=x86_64     (Intel only)
#        scons macos_arch=universal   (default: fat binary for both)
macos_arch = ARGUMENTS.get('macos_arch', 'universal')

# --- Universal GUI option (macOS only) ---
# Usage: scons gui universal_gui=1
# When set, the GUI uses GLFW from GLFW_PREFIX (e.g. built from source)
# instead of Homebrew's single-arch GLFW.
universal_gui = ARGUMENTS.get('universal_gui', '0') == '1'

# --- CRT linkage option (Windows only) ---
# Usage: scons crt=static   -> /MT (static CRT, for consumers like godot-livekit)
#        scons crt=dynamic   -> /MD (dynamic CRT, default)
crt = ARGUMENTS.get('crt', 'dynamic')
if crt not in ('static', 'dynamic'):
    print(f"Invalid crt={crt}; expected 'static' or 'dynamic'")
    Exit(1)

# --- Common C++ environment ---
if platform == 'win32' and target != 'android':
    # MSVC on Windows — use /std:c++20 instead of -std=c++20
    crt_flag = '/MT' if crt == 'static' else '/MD'
    env = Environment(
        CPPPATH=['include', 'src'],
        CXXFLAGS=['/std:c++20', '/W4', '/O2', '/EHsc', '/GS', '/sdl', crt_flag],
        # <windows.h> (pulled in by winsock for the streaming transports) defines
        # min()/max() macros that collide with std::min/std::max and
        # numeric_limits::max; suppress them library-wide.
        CPPDEFINES=['NOMINMAX', 'WIN32_LEAN_AND_MEAN',
                    '_CRT_SECURE_NO_WARNINGS'],
    )
    if sanitize == 'address':
        env.Append(CXXFLAGS=['/fsanitize=address'])
        env.Append(LINKFLAGS=['/fsanitize=address'])
else:
    _link_flags = []
    if target == 'android':
        _link_flags = ['-pie']  # Android requires PIE
    elif platform.startswith('linux'):
        _link_flags = ['-pie', '-Wl,-z,relro,-z,now']
    env = Environment(
        CPPPATH=['include', 'src'],
        CXXFLAGS=[
            '-std=c++20', '-Wall', '-Wextra', '-Wshadow',
            '-Wformat', '-Wformat-security', '-O2',
            '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2', '-fPIC',
        ],
        LINKFLAGS=_link_flags,
    )
    if target == 'android':
        # Use Android NDK compiler if CXX is not already set via environment
        ndk_cxx = os.environ.get('CXX', '')
        if ndk_cxx:
            env['CXX'] = ndk_cxx
        elif os.environ.get('ANDROID_NDK'):
            # Common NDK toolchain path — caller should set CXX explicitly
            pass
    if sanitize in ('address', 'thread', 'undefined'):
        san_flags = [f'-fsanitize={sanitize}', '-fno-omit-frame-pointer', '-g']
        env.Append(CXXFLAGS=san_flags, LINKFLAGS=san_flags)

# --- Vendored vo-aacenc (Apache-2.0) AAC-LC encoder ---
# Compiled straight into the recording targets so there is no ffmpeg/libav
# dependency for audio. Pure C/C++ (the ARM asm paths are not enabled on x86).
_voaac_root = 'vendor/vo-aacenc'
_voaac_incs = [
    _voaac_root,
    os.path.join(_voaac_root, 'aacenc/inc'),
    os.path.join(_voaac_root, 'aacenc/src'),
    os.path.join(_voaac_root, 'aacenc/basic_op'),
    os.path.join(_voaac_root, 'common/include'),
]
_voaac_srcs = [
    'common/cmnMemory.c',
    'aacenc/basic_op/basicop2.c',
    'aacenc/basic_op/oper_32b.c',
] + ['aacenc/src/%s' % f for f in [
    'aac_rom.c', 'aacenc.c', 'aacenc_core.c', 'adj_thr.c', 'band_nrg.c',
    'bit_cnt.c', 'bitbuffer.c', 'bitenc.c', 'block_switch.c', 'channel_map.c',
    'dyn_bits.c', 'grp_data.c', 'interface.c', 'line_pe.c', 'memalign.c',
    'ms_stereo.c', 'pre_echo_control.c', 'psy_configuration.c', 'psy_main.c',
    'qc_main.c', 'quantize.c', 'sf_estim.c', 'spreading.c', 'stat_bits.c',
    'tns.c', 'transform.c',
]]


def voaac_objects(base_env, objdir):
    """Compile the vendored vo-aacenc sources to objects under objdir."""
    vo = base_env.Clone()
    vo.Append(CPPPATH=_voaac_incs)
    # Third-party C: drop our strict warnings, keep position independence.
    # __unused is an Android/BSD attribute macro glibc doesn't provide.
    vo.Append(CCFLAGS=['-fPIC', '-w', '-D__unused='])
    vo.Append(CXXFLAGS=['-w'])
    objs = []
    for s in _voaac_srcs:
        stem = os.path.splitext(os.path.basename(s))[0]
        objs.append(vo.Object('%s/%s' % (objdir, stem),
                              os.path.join(_voaac_root, s)))
    return objs


# --- Vendored libsrt (SRT transport) ---
# Built from source into vendor/srt/install (encryption off -> no OpenSSL dep):
#   cmake -S vendor/srt -B vendor/srt/build -DENABLE_ENCRYPTION=OFF \
#         -DENABLE_APPS=OFF -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
#         -DENABLE_CXX_DEPS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
#         -DCMAKE_INSTALL_PREFIX=$PWD/vendor/srt/install
#   cmake --build vendor/srt/build --target srt_static -j && \
#         cmake --install vendor/srt/build
# Falls back to a system libsrt (pkg-config) when the vendored copy is absent.
_srt_install = 'vendor/srt/install'


# Static libsrt file name differs by toolchain: GNU ar vs MSVC.
_srt_lib_file = 'srt_static.lib' if platform == 'win32' else 'libsrt.a'


def _find_cmake():
    """A working cmake. On Windows, System32 sometimes shadows a broken stub
    (no CMAKE_ROOT), so prefer a real install / the VS-bundled copy."""
    import shutil
    if platform == 'win32':
        for c in (
            r'C:\Program Files\CMake\bin\cmake.exe',
            r'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7'
            r'\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        ):
            if os.path.isfile(c):
                return c
    return shutil.which('cmake')


def build_vendored_srt(label):
    """One-time cmake build of the libsrt submodule into _srt_install."""
    src = 'vendor/srt'
    if not os.path.isfile(os.path.join(src, 'CMakeLists.txt')):
        print('%s: vendor/srt not initialized (run: git submodule update '
              '--init vendor/srt) -- building without SRT.' % label)
        return
    import subprocess
    cmake = _find_cmake()
    if cmake is None:
        print('%s: cmake not found; cannot build vendored libsrt.' % label)
        return
    build_dir = os.path.join(src, 'build')
    prefix = os.path.abspath(_srt_install)
    print('%s: building vendored libsrt (one-time, ~1 min)...' % label)
    try:
        cmake_cmd = [
            cmake, '-S', src, '-B', build_dir,
            '-DENABLE_ENCRYPTION=OFF', '-DENABLE_APPS=OFF',
            '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON',
            '-DENABLE_CXX_DEPS=OFF', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            # libsrt's CMakeLists requests a pre-3.5 minimum that CMake 4
            # rejects outright; opt back into the old policy floor.
            '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
            '-DCMAKE_INSTALL_PREFIX=' + prefix,
        ]
        build_cmd = [cmake, '--build', build_dir, '--target', 'srt_static']
        install_cmd = [cmake, '--install', build_dir]
        if platform == 'win32':
            # Force a 64-bit, static-CRT (/MT) build so the lib matches the
            # GUI/CLI (which link vcpkg x64-windows-static + crt=static).
            cmake_cmd += ['-A', 'x64',
                          '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded']
            build_cmd += ['--config', 'Release']
            install_cmd += ['--config', 'Release']
        else:
            build_cmd += ['-j']
        # Match the macOS architecture the rest of the build targets so the
        # static lib isn't a single-arch mismatch at link time.
        if platform == 'darwin':
            if macos_arch == 'universal':
                cmake_cmd.append('-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64')
            elif macos_arch in ('arm64', 'x86_64'):
                cmake_cmd.append('-DCMAKE_OSX_ARCHITECTURES=' + macos_arch)
        subprocess.check_call(cmake_cmd)
        subprocess.check_call(build_cmd)
        subprocess.check_call(install_cmd)
    except Exception as e:
        print('%s: libsrt build failed (%s); continuing without SRT.'
              % (label, e))


def enable_srt(an_env, label):
    liba = os.path.join(_srt_install, 'lib', _srt_lib_file)
    if not os.path.isfile(liba):
        build_vendored_srt(label)
    if os.path.isfile(liba):
        an_env.Append(CPPPATH=[os.path.join(_srt_install, 'include')])
        an_env.Append(LIBPATH=[os.path.join(_srt_install, 'lib')])
        if platform == 'win32':
            # libsrt uses its built-in C++11 pthread shim; just needs Winsock.
            an_env.Append(LIBS=['srt_static', 'ws2_32'])
        else:
            an_env.Append(LIBS=['srt', 'pthread'])
        an_env.Append(CPPDEFINES=['FRAMETAP_HAVE_SRT'])
        return
    try:
        an_env.ParseConfig('pkg-config --cflags --libs srt')
        an_env.Append(CPPDEFINES=['FRAMETAP_HAVE_SRT'])
    except Exception:
        print('%s: libsrt not found; building without SRT (udp/rtmp work).'
              % label)


sources = ['src/frametap.cpp']

# --- Platform-specific configuration ---
if target == 'android':
    # Android: screencap-based backend, no special library dependencies.
    # Cross-compile with NDK: scons target=android CXX=/path/to/ndk-clang++
    sources += [
        'src/platform/android/android_backend.cpp',
        'src/platform/android/jni_bridge.cpp',
    ]

elif platform == 'darwin':
    env['CXX'] = 'clang++'
    # Resolve architecture flags from macos_arch option
    if macos_arch == 'universal':
        _arch_flags = ['-arch', 'arm64', '-arch', 'x86_64']
    elif macos_arch in ('arm64', 'x86_64'):
        _arch_flags = ['-arch', macos_arch]
    else:
        print(f"Invalid macos_arch={macos_arch}; expected 'universal', 'arm64', or 'x86_64'")
        Exit(1)
    # Objective-C++ for .mm files; target macOS 12.3+ (ScreenCaptureKit minimum)
    env.Append(
        CXXFLAGS=['-fobjc-arc', '-mmacosx-version-min=12.3'] + _arch_flags,
        LINKFLAGS=['-mmacosx-version-min=12.3'] + _arch_flags,
        FRAMEWORKS=[
            'Foundation',
            'AppKit',
            'ScreenCaptureKit',
            'CoreVideo',
            'CoreMedia',
            'CoreGraphics',
        ],
    )
    sources += ['src/platform/macos/macos_backend.mm']

elif platform.startswith('linux') and target != 'android':
    # X11 libraries
    env.Append(LIBS=['X11', 'Xext', 'Xfixes', 'Xinerama'])

    # PipeWire (Wayland backend)
    env.ParseConfig('pkg-config --cflags --libs libpipewire-0.3')

    # sd-bus from libsystemd (portal D-Bus communication)
    env.ParseConfig('pkg-config --cflags --libs libsystemd')

    # Wayland client (monitor enumeration)
    env.ParseConfig('pkg-config --cflags --libs wayland-client')

    sources += [
        # Runtime dispatch
        'src/platform/linux/linux_backend.cpp',
        # X11 sub-backend
        'src/platform/linux/x11/x11_backend.cpp',
        'src/platform/linux/x11/x11_screenshot.cpp',
        'src/platform/linux/x11/x11_enumerate.cpp',
        # Wayland sub-backend (portal + PipeWire)
        'src/platform/linux/wayland/wl_backend.cpp',
        'src/platform/linux/wayland/wl_portal.cpp',
        'src/platform/linux/wayland/wl_enumerate.cpp',
    ]

elif platform == 'win32':
    env.Append(
        LIBS=['dxgi', 'd3d11', 'dwmapi', 'user32', 'gdi32', 'ole32'],
    )
    sources += [
        'src/platform/windows/windows_backend.cpp',
        'src/platform/windows/windows_screenshot.cpp',
        'src/platform/windows/windows_enumerate.cpp',
    ]

else:
    print(f'Unsupported platform: {platform}')
    Exit(1)

# --- Static library ---
lib = env.StaticLibrary('frametap', sources)
Default(lib)

# --- Example binary (optional: `scons example`) ---
example_env = env.Clone()
example_env.Prepend(LIBS=['frametap'])
example_env.Append(LIBPATH=['.'])

if platform == 'darwin':
    example_env.Append(LINKFLAGS=['-lobjc'])

example = example_env.Program(
    'examples/capture_example',
    'examples/capture_example.cpp',
)
Depends(example, lib)
Alias('example', example)

# --- Recording / streaming sources ---
# The send pipeline (capture -> encode -> mux -> file/network) is split into a
# cross-platform core plus a per-platform video encoder and audio capture:
#   Linux: NVENC (dlopen'd CUDA/NVENC) + PipeWire audio
#   macOS: VideoToolbox + ScreenCaptureKit audio
# The muxers (MP4 / MPEG-TS / FLV) and AAC encode are hand-rolled / vendored, so
# there is no ffmpeg/libav dependency on either platform.
_rec_shared_srcs = [
    'src/encode/mp4_muxer.cpp',
    'src/encode/aac_encoder.cpp',
    'src/encode/net_stream.cpp',
    'src/encode/ts_muxer.cpp',
    'src/encode/ts_sink.cpp',
    'src/encode/net_transport.cpp',
    'src/encode/nal_util.cpp',
    'src/encode/rtmp_sink.cpp',
    'src/encode/recorder.cpp',
]
_rec_linux_srcs = ['src/encode/nvenc_encoder.cpp', 'src/audio/pw_capture.cpp']
_rec_macos_srcs = ['src/encode/vt_encoder.mm', 'src/audio/ca_capture.mm']
# Windows: NVENC video, but no system-audio backend / vo-aacenc build yet, so
# audio capture is a header-only stub (null_capture.h) and AAC is the null
# implementation (swapped in below). Recordings/streams are video-only for now.
_rec_win_srcs = ['src/encode/nvenc_encoder.cpp']
# Receive path (SRT in -> TS demux -> decode -> file/preview). The TS demux and
# receiver are shared; the decoder is NVDEC on Linux, VideoToolbox on macOS.
_rec_linux_recv_srcs = [
    'src/decode/ts_demux.cpp',
    'src/decode/nvdec_decoder.cpp',
    'src/decode/receiver.cpp',
]
_rec_macos_recv_srcs = [
    'src/decode/ts_demux.cpp',
    'src/decode/vt_decoder.mm',
    'src/decode/receiver.cpp',
]
# VideoToolbox is the only framework the recording objects add; CoreMedia,
# CoreVideo and ScreenCaptureKit are already linked by the base darwin env.
_rec_macos_frameworks = ['VideoToolbox']


def recording_objects(an_env, objdir, with_receive):
    """Compile the send (+ optional receive) recording sources to objects under
    objdir. Returns the object list; the caller adds voaac_objects separately."""
    srcs = list(_rec_shared_srcs)
    if platform == 'win32':
        # vo-aacenc isn't compiled under MSVC yet; use the null AAC instead.
        srcs = ['src/encode/aac_encoder_null.cpp'
                if s.endswith('aac_encoder.cpp') else s for s in srcs]
        srcs += _rec_win_srcs
        if with_receive:
            # TS demux + receiver are shared; NVDEC decoder is ported to Windows
            # (dlopen nvcuda/nvcuvid via the same shim as the encoder).
            srcs += _rec_linux_recv_srcs
    elif platform == 'darwin':
        srcs += _rec_macos_srcs
        if with_receive:
            srcs += _rec_macos_recv_srcs
    else:
        srcs += _rec_linux_srcs
        if with_receive:
            srcs += _rec_linux_recv_srcs
    objs = []
    for s in srcs:
        stem = os.path.splitext(os.path.basename(s))[0]
        objs.append(an_env.Object('%s/%s' % (objdir, stem), s))
    return objs


def enable_nvenc_headers(an_env, label):
    """Linux: add the nv-codec-headers include path. Returns True on success."""
    nvh = 'vendor/nv-codec-headers/include'
    if os.path.isdir(nvh):
        an_env.Append(CPPPATH=[nvh])
        return True
    try:
        an_env.ParseConfig('pkg-config --cflags ffnvcodec')
        return True
    except Exception:
        return False


# --- Recording demo: `scons record` (Linux/NVENC or macOS/VideoToolbox) ---
_targets = [t.replace('\\', '/') for t in COMMAND_LINE_TARGETS]
if 'record' in _targets:
    if target == 'android' or platform == 'win32':
        print('The `record` target is Linux/NVENC or macOS/VideoToolbox only.')
        Exit(1)

    rec_env = env.Clone()
    rec_env.Prepend(LIBS=['frametap'])
    rec_env.Append(LIBPATH=['.'])
    rec_env.Append(CPPPATH=_voaac_incs)
    # SRT transport links libsrt when present (cross-platform); UDP/RTMP always.
    enable_srt(rec_env, 'record')

    if platform == 'darwin':
        rec_env.Append(LINKFLAGS=['-lobjc'])
        rec_env.Append(FRAMEWORKS=_rec_macos_frameworks)
    else:
        rec_env.Append(LIBS=['dl'])
        if not enable_nvenc_headers(rec_env, 'record'):
            print('nv-codec-headers not found. Install it with:\n'
                  '  git clone https://github.com/FFmpeg/nv-codec-headers '
                  'vendor/nv-codec-headers')
            Exit(1)

    record = rec_env.Program(
        'examples/record_example',
        recording_objects(rec_env, 'build/rec', with_receive=False)
        + ['examples/record_example.cpp']
        + voaac_objects(rec_env, 'build/voaac_rec'),
    )
    Depends(record, lib)
    Alias('record', record)

# --- CLI binary (optional: `scons cli`) ---
cli_env = env.Clone()
cli_env.Prepend(LIBS=['frametap'])
cli_env.Append(LIBPATH=['.'])

if platform == 'darwin':
    cli_env.Append(LINKFLAGS=['-lobjc'])

cli_sources = ['cli/frametap_cli.cpp']

# Optional recording/streaming in the CLI (Linux/NVENC or macOS/VideoToolbox),
# mirroring the GUI. Gated on `cli` being requested so a plain `scons`/`scons
# test` never triggers the one-time libsrt build. The send path works on both
# platforms; the receive path (NVDEC) is Linux-only.
if 'cli' in _targets and target != 'android':
    _cli_rec = False
    _cli_recv = False
    if platform == 'darwin':
        _cli_rec = True
        _cli_recv = True  # VideoToolbox decode
        cli_env.Append(FRAMEWORKS=_rec_macos_frameworks)
    elif platform.startswith('linux'):
        _cli_rec = enable_nvenc_headers(cli_env, 'CLI')
        _cli_recv = _cli_rec  # receive path rides on the NVENC build
        if _cli_rec:
            cli_env.Append(LIBS=['dl'])
    elif platform == 'win32':
        # NVENC send + NVDEC receive, video-only (see GUI note above).
        _cli_rec = enable_nvenc_headers(cli_env, 'CLI')
        _cli_recv = _cli_rec
    if _cli_rec:
        cli_env.Append(CPPDEFINES=['FRAMETAP_CLI_RECORDING'])
        if _cli_recv:
            cli_env.Append(CPPDEFINES=['FRAMETAP_CLI_RECEIVING'])
        if platform == 'win32':
            cli_env.Append(LIBS=['ws2_32'])
            enable_srt(cli_env, 'CLI')
            cli_sources += recording_objects(cli_env, 'cli/obj',
                                             with_receive=_cli_recv)
        else:
            cli_env.Append(CPPPATH=_voaac_incs)
            enable_srt(cli_env, 'CLI')
            cli_sources += (
                recording_objects(cli_env, 'cli/obj', with_receive=_cli_recv)
                + voaac_objects(cli_env, 'cli/obj/voaac'))

cli = cli_env.Program('cli/frametap', cli_sources)
Depends(cli, lib)
Alias('cli', cli)

# --- GUI binary (optional: `scons gui`) ---
_targets = [t.replace('\\', '/') for t in COMMAND_LINE_TARGETS]
build_gui = 'gui' in _targets

if build_gui:
    gui_env = env.Clone()
    gui_env.Prepend(LIBS=['frametap'])
    gui_env.Append(LIBPATH=['.'])
    gui_env.Append(CPPPATH=[
        'vendor/imgui',
        'vendor/imgui/backends',
        'vendor/lodepng',
    ])

    imgui_sources = [
        'vendor/imgui/imgui.cpp',
        'vendor/imgui/imgui_draw.cpp',
        'vendor/imgui/imgui_tables.cpp',
        'vendor/imgui/imgui_widgets.cpp',
        'vendor/imgui/imgui_demo.cpp',
        'vendor/imgui/backends/imgui_impl_glfw.cpp',
        'vendor/imgui/backends/imgui_impl_opengl3.cpp',
    ]
    gui_sources = ['gui/frametap_gui.cpp', 'vendor/lodepng/lodepng.cpp'] + imgui_sources

    # Optional recording/streaming (Linux/NVENC or macOS/VideoToolbox). When
    # available we compile the encoder into GUI-private objects (so they never
    # collide with the `record` target's objects) and define
    # FRAMETAP_GUI_RECORDING so the Record/Stream controls are built. The
    # receive path (NVDEC) is Linux-only, gated by FRAMETAP_GUI_RECEIVING.
    _gui_rec = False
    _gui_recv = False
    if platform == 'darwin':
        _gui_rec = True
        _gui_recv = True  # VideoToolbox decode
        gui_env.Append(FRAMEWORKS=_rec_macos_frameworks)
    elif platform.startswith('linux') and target != 'android':
        _gui_rec = enable_nvenc_headers(gui_env, 'GUI')
        _gui_recv = _gui_rec
        if _gui_rec:
            gui_env.Append(LIBS=['dl'])
        else:
            print('GUI: nv-codec-headers not found; building without GPU '
                  'recording. To enable it:\n'
                  '  git clone https://github.com/FFmpeg/nv-codec-headers '
                  'vendor/nv-codec-headers')
    elif platform == 'win32':
        # NVENC send + NVDEC receive (video-only: no PipeWire/vo-aacenc port
        # yet). SRT rides on the vendored libsrt build below; UDP/RTMP/file
        # work even without it.
        _gui_rec = enable_nvenc_headers(gui_env, 'GUI')
        _gui_recv = _gui_rec  # NVDEC decode is ported to Windows
        if not _gui_rec:
            print('GUI: nv-codec-headers not found; building without GPU '
                  'recording. Clone it with:\n'
                  '  git clone https://github.com/FFmpeg/nv-codec-headers '
                  'vendor/nv-codec-headers')
    if _gui_rec:
        gui_env.Append(CPPDEFINES=['FRAMETAP_GUI_RECORDING'])
        if _gui_recv:
            gui_env.Append(CPPDEFINES=['FRAMETAP_GUI_RECEIVING'])
        if platform == 'win32':
            # Winsock for the streaming transports; null AAC means no voaac.
            gui_env.Append(LIBS=['ws2_32'])
            enable_srt(gui_env, 'GUI')  # SRT send + receive when libsrt present
            gui_sources = (
                gui_sources
                + recording_objects(gui_env, 'gui/obj',
                                    with_receive=_gui_recv))
        else:
            gui_env.Append(CPPPATH=_voaac_incs)
            enable_srt(gui_env, 'GUI')
            gui_sources = (
                gui_sources
                + recording_objects(gui_env, 'gui/obj', with_receive=_gui_recv)
                + voaac_objects(gui_env, 'gui/obj/voaac'))

    if platform == 'darwin':
        gui_env.Append(LINKFLAGS=['-lobjc'])
        if universal_gui:
            # Universal build: use GLFW built from source.  The caller must
            # set GLFW_PREFIX to the install prefix (contains include/ and
            # lib/).  We skip pkg-config entirely to avoid PATH issues in CI.
            glfw_prefix = os.environ.get('GLFW_PREFIX', '')
            if not glfw_prefix:
                print('universal_gui=1 requires GLFW_PREFIX env var '
                      '(path to universal GLFW install prefix)')
                Exit(1)
            gui_env.Append(CPPPATH=[os.path.join(glfw_prefix, 'include')])
            gui_env.Append(LIBPATH=[os.path.join(glfw_prefix, 'lib')])
            gui_env.Append(LIBS=['glfw3'])
            gui_env.Append(FRAMEWORKS=['Cocoa', 'IOKit', 'CoreFoundation'])
        else:
            # Strip multi-arch flags — GLFW from Homebrew is host-arch only
            for flag_list in ('CXXFLAGS', 'LINKFLAGS'):
                flags = gui_env[flag_list]
                cleaned = []
                skip_next = False
                for f in flags:
                    if skip_next:
                        skip_next = False
                        continue
                    if f == '-arch':
                        skip_next = True
                        continue
                    cleaned.append(f)
                gui_env[flag_list] = cleaned
            gui_env.ParseConfig('pkg-config --cflags --libs glfw3')
        gui_env.Append(FRAMEWORKS=['OpenGL'])
    elif platform.startswith('linux'):
        gui_env.ParseConfig('pkg-config --cflags --libs glfw3')
        gui_env.Append(LIBS=['GL'])
    elif platform == 'win32':
        vcpkg_root = os.environ.get('VCPKG_ROOT', os.environ.get('VCPKG_INSTALLATION_ROOT', ''))
        if vcpkg_root:
            triplet = 'x64-windows-static'
            vcpkg_installed = os.path.join(vcpkg_root, 'installed', triplet)
            gui_env.Append(CPPPATH=[os.path.join(vcpkg_installed, 'include')])
            gui_env.Append(LIBPATH=[os.path.join(vcpkg_installed, 'lib')])
        gui_env.Append(LIBS=['glfw3', 'opengl32'])

    gui = gui_env.Program('gui/frametap_gui', gui_sources)
    Depends(gui, lib)
    Alias('gui', gui)

# --- Test binary (optional: `scons test` or `scons tests/test_runner`) ---
# Normalise path separators so `tests\test_runner` on Windows still matches.
_targets = [t.replace('\\', '/') for t in COMMAND_LINE_TARGETS]
build_tests = any(t in _targets for t in ['test', 'tests/test_runner'])

if build_tests:
    test_env = env.Clone()
    test_env.Prepend(LIBS=['frametap'])
    test_env.Append(LIBPATH=['.'])
    test_env.Append(CPPPATH=['cli'])

    if platform == 'darwin':
        test_env.Append(LINKFLAGS=['-lobjc'])
        # Tests don't need to be universal binaries; Homebrew Catch2 is
        # only built for the host architecture.  Strip multi-arch flags
        # so the test binary links against the native Catch2.
        for flag_list in ('CXXFLAGS', 'LINKFLAGS'):
            flags = test_env[flag_list]
            cleaned = []
            skip_next = False
            for f in flags:
                if skip_next:
                    skip_next = False
                    continue
                if f == '-arch':
                    skip_next = True
                    continue
                cleaned.append(f)
            test_env[flag_list] = cleaned

    # Catch2 via pkg-config
    if platform == 'win32':
        # Catch2Main.lib provides main() inside a static library; the MSVC
        # linker cannot auto-detect the subsystem from a .lib, so we must
        # explicitly request a console application.
        test_env.Append(LINKFLAGS=['/SUBSYSTEM:CONSOLE'])
        # On Windows, assume Catch2 is installed via vcpkg
        vcpkg_root = os.environ.get('VCPKG_ROOT', os.environ.get('VCPKG_INSTALLATION_ROOT', ''))
        if vcpkg_root:
            triplet = 'x64-windows-static' if crt == 'static' else 'x64-windows'
            vcpkg_installed = os.path.join(vcpkg_root, 'installed', triplet)
            test_env.Append(CPPPATH=[os.path.join(vcpkg_installed, 'include')])
            test_env.Append(LIBPATH=[
                os.path.join(vcpkg_installed, 'lib'),
                # vcpkg places Catch2WithMain.lib under manual-link/
                os.path.join(vcpkg_installed, 'lib', 'manual-link'),
            ])
        # Catch2 v3 via vcpkg names the main-provider lib "Catch2Main";
        # older builds used "Catch2WithMain".  Probe for the actual name.
        _main_lib = 'Catch2WithMain'
        if vcpkg_root:
            import glob as _glob
            _manual = os.path.join(vcpkg_installed, 'lib', 'manual-link')
            if _glob.glob(os.path.join(_manual, 'Catch2Main*')):
                _main_lib = 'Catch2Main'
        test_env.Append(LIBS=[_main_lib, 'Catch2'])
    else:
        test_env.ParseConfig('pkg-config --cflags --libs catch2-with-main')

    # Muxer sources under test, compiled to test-private objects so they don't
    # collide with the record/gui targets' objects.
    test_sources = Glob('tests/test_*.cpp') + [
        test_env.Object('tests/obj/ts_muxer', 'src/encode/ts_muxer.cpp'),
        test_env.Object('tests/obj/nal_util', 'src/encode/nal_util.cpp'),
        test_env.Object('tests/obj/mp4_muxer', 'src/encode/mp4_muxer.cpp'),
        test_env.Object('tests/obj/ts_demux', 'src/decode/ts_demux.cpp'),
    ]
    test_runner = test_env.Program('tests/test_runner', test_sources)
    Depends(test_runner, lib)
    # On Windows the Program target is tests/test_runner.exe, so SCons can't
    # resolve the bare `tests/test_runner` from the command line. Register an
    # explicit alias so `scons tests/test_runner` works on all platforms.
    Alias('tests/test_runner', test_runner)

    # `scons test` builds and runs the test suite
    test_run = test_env.Command(
        'test_run_stamp',
        test_runner,
        os.path.join('.', 'tests', 'test_runner') + ' --colour-mode ansi',
    )
    AlwaysBuild(test_run)
    Alias('test', test_run)
