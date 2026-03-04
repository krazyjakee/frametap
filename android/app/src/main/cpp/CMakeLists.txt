cmake_minimum_required(VERSION 3.22.1)
project(frametap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the frametap project root (relative to this CMakeLists.txt)
set(FRAMETAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")

add_library(frametap SHARED
    # Core
    ${FRAMETAP_ROOT}/src/frametap.cpp

    # Android backend
    ${FRAMETAP_ROOT}/src/platform/android/android_backend.cpp
    ${FRAMETAP_ROOT}/src/platform/android/jni_bridge.cpp

    # GUI JNI entry point
    gui_jni.cpp
)

target_include_directories(frametap PRIVATE
    ${FRAMETAP_ROOT}/include
    ${FRAMETAP_ROOT}/src
)

target_compile_definitions(frametap PRIVATE __ANDROID__)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(frametap
    ${log-lib}
    ${android-lib}
)
