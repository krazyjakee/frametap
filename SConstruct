import sys
import os

# --- Platform detection ---
platform = sys.platform  # 'darwin', 'linux', 'win32'

# --- Sanitizer option ---
# Usage: scons sanitize=address
#        scons sanitize=thread
#        scons sanitize=undefined
sanitize = ARGUMENTS.get('sanitize', 'none')

# --- Universal GUI option (macOS only) ---
# Usage: scons gui universal_gui=1
# When set, the GUI keeps multi-arch flags (-arch arm64 -arch x86_64).
# Requires a universal GLFW (e.g. built from source); Homebrew GLFW is
# single-arch so this is off by default.
universal_gui = ARGUMENTS.get('universal_gui', '0') == '1'

# --- CRT linkage option (Windows only) ---
# Usage: scons crt=static   -> /MT (static CRT, for consumers like godot-livekit)
#        scons crt=dynamic   -> /MD (dynamic CRT, default)
crt = ARGUMENTS.get('crt', 'dynamic')
if crt not in ('static', 'dynamic'):
    print(f"Invalid crt={crt}; expected 'static' or 'dynamic'")
    Exit(1)

# --- Common C++ environment ---
if platform == 'win32':
    # MSVC on Windows — use /std:c++20 instead of -std=c++20
    crt_flag = '/MT' if crt == 'static' else '/MD'
    env = Environment(
        CPPPATH=['include', 'src'],
        CXXFLAGS=['/std:c++20', '/W4', '/O2', '/EHsc', '/GS', '/sdl', crt_flag],
    )
    if sanitize == 'address':
        env.Append(CXXFLAGS=['/fsanitize=address'])
        env.Append(LINKFLAGS=['/fsanitize=address'])
else:
    env = Environment(
        CPPPATH=['include', 'src'],
        CXXFLAGS=[
            '-std=c++20', '-Wall', '-Wextra', '-Wshadow',
            '-Wformat', '-Wformat-security', '-O2',
            '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2', '-fPIC',
        ],
        LINKFLAGS=['-pie', '-Wl,-z,relro,-z,now'] if platform.startswith('linux') else [],
    )
    if sanitize in ('address', 'thread', 'undefined'):
        san_flags = [f'-fsanitize={sanitize}', '-fno-omit-frame-pointer', '-g']
        env.Append(CXXFLAGS=san_flags, LINKFLAGS=san_flags)

sources = ['src/frametap.cpp']

# --- Platform-specific configuration ---
if platform == 'darwin':
    env['CXX'] = 'clang++'
    # Objective-C++ for .mm files; target macOS 12.3+ (ScreenCaptureKit minimum)
    env.Append(
        CXXFLAGS=['-fobjc-arc', '-mmacosx-version-min=12.3',
                  '-arch', 'arm64', '-arch', 'x86_64'],
        LINKFLAGS=['-mmacosx-version-min=12.3',
                   '-arch', 'arm64', '-arch', 'x86_64'],
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

elif platform.startswith('linux'):
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

# --- CLI binary (optional: `scons cli`) ---
cli_env = env.Clone()
cli_env.Prepend(LIBS=['frametap'])
cli_env.Append(LIBPATH=['.'])

if platform == 'darwin':
    cli_env.Append(LINKFLAGS=['-lobjc'])

cli = cli_env.Program(
    'cli/frametap',
    'cli/frametap_cli.cpp',
)
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

    test_sources = Glob('tests/test_*.cpp')
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
