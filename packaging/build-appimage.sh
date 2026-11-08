#!/usr/bin/env bash
# Builds Raccord-<version>-x86_64.AppImage from a configured build directory.
# Usage: packaging/build-appimage.sh [build dir]   (default: build)
# Needs appimagetool in PATH or in .tools/; without it, stops after the AppDir.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$SRC/build}" && pwd)"
VERSION="$(sed -n 's/^project(Raccord VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
APPDIR="$BUILD/AppDir"
QTPLUG="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib/x86_64-linux-gnu/qt6/plugins)"
QTTR="$(qtpaths6 --query QT_INSTALL_TRANSLATIONS 2>/dev/null || echo /usr/share/qt6/translations)"

cmake --build "$BUILD"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr >/dev/null

# Qt plugins needed at runtime.
mkdir -p "$APPDIR/usr/plugins"
for d in platforms xcbglintegrations platformthemes platforminputcontexts imageformats iconengines multimedia tls wayland-shell-integration wayland-graphics-integration-client wayland-decoration-client egldeviceintegrations; do
  [ -d "$QTPLUG/$d" ] && cp -a "$QTPLUG/$d" "$APPDIR/usr/plugins/"
done
mkdir -p "$APPDIR/usr/translations"
cp "$QTTR"/qtbase_*.qm "$APPDIR/usr/translations/" 2>/dev/null || true
cat > "$APPDIR/usr/bin/qt.conf" <<QT
[Paths]
Prefix = ..
Plugins = plugins
Translations = translations
QT

# Libraries the target system is expected to provide (AppImage excludelist).
EXCLUDE='^(ld-linux|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libresolv\.so|libutil\.so|libstdc\+\+|libgcc_s|libGL\.so|libGLX|libGLdispatch|libEGL\.so|libOpenGL|libdrm\.so|libgbm|libX11\.so|libX11-xcb|libxcb\.so|libxcb-dri|libxcb-glx|libxcb-present|libxcb-sync|libxshmfence|libasound|libpulse|libjack|libpipewire|libfontconfig|libfreetype|libharfbuzz|libexpat|libz\.so|libuuid|libgpg-error|libcom_err|libgssapi|libkrb5|libk5crypto|libnss|libnspr|libva|libvdpau|libcuda|libnvidia|libdbus-1|libsystemd|libudev|libwayland-client|libwayland-egl|libwayland-cursor)'
mkdir -p "$APPDIR/usr/lib"
copy_deps() {
  ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r lib; do
    base="$(basename "$lib")"
    [[ "$base" =~ $EXCLUDE ]] && continue
    [ -e "$APPDIR/usr/lib/$base" ] && continue
    cp -L "$lib" "$APPDIR/usr/lib/$base"
  done
}
for f in "$APPDIR"/usr/bin/raccord "$APPDIR"/usr/bin/raccord-cli $(find "$APPDIR/usr/plugins" -name '*.so'); do copy_deps "$f"; done
# Second pass for transitive deps of the copied libraries.
for f in "$APPDIR"/usr/lib/*.so*; do copy_deps "$f"; done
command -v patchelf >/dev/null && for f in "$APPDIR"/usr/bin/raccord "$APPDIR"/usr/bin/raccord-cli; do patchelf --set-rpath '$ORIGIN/../lib' "$f"; done

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
if [ "$1" = "--cli" ]; then shift; exec "$HERE/usr/bin/raccord-cli" "$@"; fi
exec "$HERE/usr/bin/raccord" "$@"
RUN
chmod +x "$APPDIR/AppRun"
cp "$APPDIR/usr/share/applications/raccord.desktop" "$APPDIR/"
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/raccord.svg" "$APPDIR/raccord.svg"
ln -sf raccord.svg "$APPDIR/.DirIcon"

TOOL="$(command -v appimagetool || true)"
[ -z "$TOOL" ] && [ -x "$SRC/.tools/appimagetool-x86_64.AppImage" ] && TOOL="$SRC/.tools/appimagetool-x86_64.AppImage"
if [ -z "$TOOL" ]; then
  echo "AppDir ready in $APPDIR. Install appimagetool (https://github.com/AppImage/appimagetool) to pack it." >&2
  exit 0
fi
OUT="$BUILD/Raccord-$VERSION-x86_64.AppImage"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" -n "$APPDIR" "$OUT"
echo "$OUT"
