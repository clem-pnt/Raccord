cmake_minimum_required(VERSION 3.20)
project(Raccord VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RACCORD_BUILD_TESTS "Build unit tests" ON)
option(RACCORD_NATIVE "Optimize for the build machine (-march=native)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
    if(RACCORD_NATIVE)
      add_compile_options(-march=native)
    endif()
  endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia Concurrent Xml)
if(WIN32 AND RACCORD_FFMPEG_WIN)
  # Prebuilt shared FFmpeg (include/, lib/*.dll.a, bin/*.dll).
  add_library(PkgConfig::FFMPEG INTERFACE IMPORTED)
  target_include_directories(PkgConfig::FFMPEG INTERFACE ${RACCORD_FFMPEG_WIN}/include)
  foreach(l avformat avcodec avutil swscale swresample avfilter)
    target_link_libraries(PkgConfig::FFMPEG INTERFACE ${RACCORD_FFMPEG_WIN}/lib/lib${l}.dll.a)
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample libavfilter)
endif()
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

configure_file(src/core/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

add_subdirectory(src)
add_subdirectory(app)
add_subdirectory(cli)

if(RACCORD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(packaging/Packaging.cmake)
