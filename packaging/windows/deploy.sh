#!/usr/bin/env bash
# Gathers a self-contained Windows folder: executables, Qt plugins, DLLs
# (resolved recursively with objdump), translations.
# Usage: deploy.sh <windows build dir> <output dir> <Qt mingw_64 dir> <FFmpeg dir> [python embed dir]
set -euo pipefail
BUILD="$1"; OUT="$2"; QT="$3"; FF="$4"; PY="${5:-}"
OBJDUMP="env LC_ALL=C x86_64-w64-mingw32-objdump"
GCCDIR="$(dirname "$(x86_64-w64-mingw32-g++-posix -print-libgcc-file-name)")"
# Search order: our compiler runtime first (newer libstdc++), then FFmpeg, Qt, sysroot.
SEARCH=("$GCCDIR" /usr/x86_64-w64-mingw32/lib "$FF/bin" "$QT/bin")
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/app/raccord.exe" "$BUILD/cli/raccord-cli.exe" "$OUT/"
for d in platforms/qwindows.dll styles/qwindowsvistastyle.dll imageformats iconengines multimedia/ffmpegmediaplugin.dll multimedia/windowsmediaplugin.dll tls generic; do
  src="$QT/plugins/$d"
  if [ -d "$src" ]; then mkdir -p "$OUT/$d"; cp "$src"/*.dll "$OUT/$d/" 2>/dev/null || true; rm -f "$OUT/$d"/*d.dll.debug
  elif [ -f "$src" ]; then mkdir -p "$OUT/$(dirname "$d")"; cp "$src" "$OUT/$d"; fi
done
cp "$QT/bin/opengl32sw.dll" "$QT/bin/d3dcompiler_47.dll" "$OUT/"
mkdir -p "$OUT/translations"
cp "$QT"/translations/qtbase_*.qm "$OUT/translations/" 2>/dev/null || true
printf '[Paths]\nPrefix = .\nPlugins = .\nTranslations = translations\n' > "$OUT/qt.conf"

is_system() {
  case "${1,,}" in
    kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|ws2_32.dll|msvcrt.dll|ucrtbase.dll|api-ms-win-*|\
    opengl32.dll|winmm.dll|version.dll|comdlg32.dll|imm32.dll|shlwapi.dll|uxtheme.dll|dwmapi.dll|mpr.dll|netapi32.dll|userenv.dll|\
    wtsapi32.dll|setupapi.dll|crypt32.dll|bcrypt.dll|secur32.dll|d3d11.dll|d3d12.dll|dxgi.dll|d3d9.dll|dxva2.dll|mf.dll|mfplat.dll|\
    mfreadwrite.dll|mfuuid.dll|strmiids.dll|evr.dll|ksuser.dll|avrt.dll|propsys.dll|wmcodecdspuuid.dll|ntdll.dll|iphlpapi.dll|dnsapi.dll|\
    authz.dll|normaliz.dll|comctl32.dll|winspool.drv|rpcrt4.dll|dbghelp.dll|psapi.dll|powrprof.dll|cfgmgr32.dll|hid.dll|vfw32.dll|\
    msacm32.dll|dwrite.dll|d2d1.dll|shcore.dll|wldap32.dll|dcomp.dll|mfcore.dll|dsound.dll|bcryptprimitives.dll|cabinet.dll|\
    mscms.dll|ncrypt.dll|oleacc.dll|wininet.dll|winhttp.dll|usp10.dll|msimg32.dll|glu32.dll|gdiplus.dll|ole32.dll|coremessaging.dll) return 0;;
  esac
  return 1
}
declare -A seen
resolve() {
  local f="$1"
  for dll in $($OBJDUMP -p "$f" | sed -n 's/^\s*DLL Name: //p'); do
    is_system "$dll" && continue
    [ -n "${seen[${dll,,}]:-}" ] && continue
    seen[${dll,,}]=1
    local found=""
    for dir in "${SEARCH[@]}"; do
      if [ -f "$dir/$dll" ]; then found="$dir/$dll"; break; fi
    done
    if [ -z "$found" ]; then echo "warning: $dll not found (needed by $(basename "$f"))" >&2; continue; fi
    [ -f "$OUT/$dll" ] || cp "$found" "$OUT/$dll"
    resolve "$OUT/$dll"
  done
}
for f in "$OUT"/*.exe $(find "$OUT" -mindepth 2 -name '*.dll'); do resolve "$f"; done
if [ -n "$PY" ] && [ -d "$PY" ]; then cp -r "$PY" "$OUT/python"; fi
echo "$OUT: $(du -sh "$OUT" | cut -f1)"
