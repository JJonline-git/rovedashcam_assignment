cmake_minimum_required(VERSION 3.16)
project(isp-channel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All dependencies are found through pkg-config (works the same natively and in the Yocto sysroot).
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

add_executable(isp-channel
    src/main.cpp
    src/camera_channel.cpp
    src/encoder.cpp
    src/snapshot.cpp
    src/mapped_buffer.cpp
    src/stats.cpp)

target_compile_options(isp-channel PRIVATE -Wall -Wextra)
target_link_libraries(isp-channel PRIVATE
    PkgConfig::LIBCAMERA PkgConfig::GST PkgConfig::PNG Threads::Threads)

install(TARGETS isp-channel RUNTIME DESTINATION bin)
