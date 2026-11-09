#!/usr/bin/env bash
# Cross-builds Raccord for Windows on Debian/Ubuntu and produces:
#   build-win/Raccord-<version>-Setup.exe  (installer)
#   build-win/Raccord-<version>-win64.zip  (portable)
# Dependencies in .tools/win (see docs): Qt 6.5.3 mingw_64 + gcc_64 host tools
# (aqtinstall), FFmpeg shared build, Python embeddable + get-pip + MSVC runtime, catch2 header.
# apt: mingw-w64 libz-mingw-w64-dev nsis p7zip-full
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
T="$SRC/.tools/win"
VERSION="$(sed -n 's/^project(Raccord VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
FF="$(ls -d "$T"/ffx/*/ | head -1)"; FF="${FF%/}"
BUILD="$SRC/build-win"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$SRC/packaging/windows/mingw-toolchain.cmake" \
  -DRACCORD_QT_WIN="$T/Qt/6.5.3/mingw_64" -DQT_HOST_PATH="$T/QtHost/6.5.3/gcc_64" \
  -DRACCORD_FFMPEG_WIN="$FF" -DRACCORD_CATCH2_INCLUDE="$T/catch2" -DRACCORD_BUILD_TESTS=ON >/dev/null
cmake --build "$BUILD"
"$SRC/packaging/windows/deploy.sh" "$BUILD" "$BUILD/Raccord" "$T/Qt/6.5.3/mingw_64" "$FF" "$T/python"
# Strip debug symbols from our binaries.
x86_64-w64-mingw32-strip "$BUILD/Raccord/raccord.exe" "$BUILD/Raccord/raccord-cli.exe"
cp "$SRC/README.md" "$BUILD/Raccord/README.txt"
cp "$FF/LICENSE" "$BUILD/Raccord/FFMPEG-LICENSE.txt" 2>/dev/null || true
( cd "$SRC/packaging/windows" && makensis -V2 -DVERSION="$VERSION" -DSRCDIR="$BUILD/Raccord" \
    -DOUTFILE="$BUILD/Raccord-$VERSION-Setup.exe" installer.nsi )
rm -f "$BUILD/Raccord-$VERSION-win64.zip"
( cd "$BUILD" && 7z a -tzip -mx=7 "Raccord-$VERSION-win64.zip" Raccord >/dev/null )
ls -la "$BUILD/Raccord-$VERSION-Setup.exe" "$BUILD/Raccord-$VERSION-win64.zip"
