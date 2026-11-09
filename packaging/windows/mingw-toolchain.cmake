# Cross compilation for Windows x64 with MinGW-w64 (Debian: mingw-w64 package).
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-toolchain.cmake \
#   -DRACCORD_QT_WIN=<Qt>/6.5.3/mingw_64 -DQT_HOST_PATH=<Qt>/6.5.3/gcc_64 -DRACCORD_FFMPEG_WIN=<ffmpeg shared dir> ...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${RACCORD_QT_WIN} ${RACCORD_FFMPEG_WIN})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
list(APPEND CMAKE_PREFIX_PATH ${RACCORD_QT_WIN})
# Never pick up the host's pkg-config files.
set(ENV{PKG_CONFIG_LIBDIR} "")
set(ENV{PKG_CONFIG_PATH} "")
