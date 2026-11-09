#!/usr/bin/env bash
# Builds a self-contained .deb (Qt and FFmpeg bundled in /opt/raccord) that does not
# depend on the FFmpeg version of the target distribution.
# Usage: packaging/build-deb.sh [build dir] [package revision]
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$SRC/build}" && pwd)"
REV="${2:-1}"
VERSION="$(sed -n 's/^project(Raccord VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
[ -d "$BUILD/AppDir/usr/lib" ] || "$SRC/packaging/build-appimage.sh" "$BUILD"
command -v patchelf >/dev/null || { echo "patchelf is required" >&2; exit 1; }

PKG="$BUILD/deb-root"
OPT="$PKG/opt/raccord"
rm -rf "$PKG"
mkdir -p "$OPT" "$PKG/usr/bin" "$PKG/DEBIAN"
cp -a "$BUILD/AppDir/usr/bin" "$BUILD/AppDir/usr/lib" "$BUILD/AppDir/usr/plugins" "$BUILD/AppDir/usr/translations" "$OPT/"
cp -a "$BUILD/AppDir/usr/share" "$PKG/usr/"
ln -s /opt/raccord/bin/raccord "$PKG/usr/bin/raccord"
ln -s /opt/raccord/bin/raccord-cli "$PKG/usr/bin/raccord-cli"

# RUNPATH is not transitive: every bundled object needs its own.
patchelf --set-rpath '$ORIGIN/../lib' "$OPT/bin/raccord" "$OPT/bin/raccord-cli"
find "$OPT/lib" -type f -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} +
find "$OPT/plugins" -type f -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} +

# System packages providing the libraries left out of the bundle: only what bundled
# objects link directly, the dependencies of those packages are apt's business.
NEEDED="$(find "$OPT" -type f \( -name '*.so*' -o -path '*/bin/raccord*' \) -print0 \
  | xargs -0 -n1 objdump -p 2>/dev/null | awk '/NEEDED/{print $2}' | sort -u)"
PKGS="$(for n in $NEEDED; do
  [ -e "$OPT/lib/$n" ] && continue
  f="$(/sbin/ldconfig -p | awk -v n="$n" '$1==n && /x86-64/{print $NF; exit}')"
  [ -n "$f" ] || { echo "unresolved library: $n" >&2; exit 1; }
  # Merged /usr: dpkg may know the file under /lib rather than /usr/lib.
  real="$(readlink -f "$f")"
  owner="$(dpkg -S "$real" 2>/dev/null || dpkg -S "${real#/usr}" 2>/dev/null || true)"
  [ -n "$owner" ] || { echo "no package owns $real" >&2; exit 1; }
  echo "${owner%%:*}"
done | sort -u | { grep -v "^libc6$" || true; })"
DEPENDS="libc6 (>= 2.36)"
for p in $PKGS; do DEPENDS="$DEPENDS, $p"; done

cat > "$PKG/DEBIAN/control" <<CTRL
Package: raccord
Version: $VERSION-$REV
Section: video
Priority: optional
Architecture: amd64
Maintainer: Raccord <noreply@raccord.invalid>
Depends: $DEPENDS
Recommends: python3-venv
Installed-Size: $(du -sk "$PKG" | cut -f1)
Description: Professional non-linear video editor
 Multitrack editing, multicam, GPU effects, colour grading, automatic subtitles
 and Premiere Pro project exchange. Qt and FFmpeg are bundled.
CTRL
OUT="$BUILD/raccord_${VERSION}-${REV}_amd64.deb"
dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT" >/dev/null
ls -la "$OUT"
