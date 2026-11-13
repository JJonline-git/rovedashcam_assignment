cmake_minimum_required(VERSION 3.16)
# C for the application; C++ only for camera.cpp (libcamera has no C API).
project(isp-channel LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)          # libcamera's headers need C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies via pkg-config: works natively and inside the Yocto sysroot.
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

add_executable(isp-channel
    src/main.c
    src/encoder.c
    src/snapshot.c
    src/stats.c
    src/camera.cpp)

target_compile_options(isp-channel PRIVATE -Wall -Wextra)
target_link_libraries(isp-channel PRIVATE
    PkgConfig::LIBCAMERA PkgConfig::GST PkgConfig::PNG Threads::Threads)

install(TARGETS isp-channel RUNTIME DESTINATION bin)
