cmake_minimum_required(VERSION 3.18)
project(anland_consumer C)

set(ANLAND_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/anland_core)

add_library(anland_consumer SHARED
    native_consumer.c
    native_audio.c
    camera_service.c
    ${ANLAND_CORE_DIR}/libdisplay_consumer/display_consumer.c
    ${ANLAND_CORE_DIR}/common/socket_utils.c
)

target_include_directories(anland_consumer PRIVATE
    ${ANLAND_CORE_DIR}/libdisplay_consumer
    ${ANLAND_CORE_DIR}/common
)

target_compile_options(anland_consumer PRIVATE -O3 -flto)
target_link_options(anland_consumer PRIVATE -flto -fuse-ld=lld)

target_link_libraries(anland_consumer
    android
    log
    dl
    aaudio
    camera2ndk
    mediandk
)

# Root helper for "connect with root" mode. Built as an executable but named
# lib*.so so the Android packager extracts it into the app's nativeLibraryDir
# with execute permission (requires useLegacyPackaging=true). Launched by the
# app via `su -c` to connect the daemon socket and pass the fd back.
add_executable(fdhelper
    fd_helper.c
    ${ANLAND_CORE_DIR}/common/socket_utils.c
)

target_include_directories(fdhelper PRIVATE
    ${ANLAND_CORE_DIR}/common
)

set_target_properties(fdhelper PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "fdhelper"
    SUFFIX ".so"
)

target_link_libraries(fdhelper log)
