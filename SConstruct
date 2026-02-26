import sys
import os

# --- Platform detection ---
platform = sys.platform  # 'darwin', 'linux', 'win32'

# --- Sanitizer option ---
# Usage: scons sanitize=address
#        scons sanitize=thread
#        scons sanitize=undefined
sanitize = ARGUMENTS.get('sanitize', 'none')

# --- Common C++ environment ---
if platform == 'win32':
    # MSVC on Windows — use /std:c++20 instead of -std=c++20
    env = Environment(
        CPPPATH=['include', 'src'],
        CXXFLAGS=['/std:c++20', '/W4', '/O2', '/EHsc', '/GS', '/sdl'],
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
        # On Windows, assume Catch2 is installed via vcpkg
        vcpkg_root = os.environ.get('VCPKG_ROOT', os.environ.get('VCPKG_INSTALLATION_ROOT', ''))
        if vcpkg_root:
            triplet = 'x64-windows'
            vcpkg_installed = os.path.join(vcpkg_root, 'installed', triplet)
            test_env.Append(CPPPATH=[os.path.join(vcpkg_installed, 'include')])
            test_env.Append(LIBPATH=[
                os.path.join(vcpkg_installed, 'lib'),
                # vcpkg places Catch2WithMain.lib under manual-link/
                os.path.join(vcpkg_installed, 'lib', 'manual-link'),
            ])
        test_env.Append(LIBS=['Catch2WithMain', 'Catch2'])
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
