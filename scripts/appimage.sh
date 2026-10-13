#!/usr/bin/env bash
# builds dist/bardown-<version>-x86_64.AppImage from the release build. no game data goes in it: the player's
# own disc image sits beside it and is checked and extracted on first launch
#   APPIMAGETOOL=/path/to/appimagetool scripts/appimage.sh [version]
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
build="${BARDOWN_BUILD:-$root/out/build/linux-amd64-release}"
version="${1:-$(git -C "$root" describe --tags --always --dirty)}"
tool="${APPIMAGETOOL:-appimagetool}"
command -v "$tool" > /dev/null || { echo "appimagetool not found: set APPIMAGETOOL"; exit 1; }

appdir="$root/out/appdir"
rm -rf "${appdir:?}"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/512x512/apps"
for f in bardown librexruntime.so librexgpu-xenos.so librexgpu-native.so; do
  install -m 755 "$build/$f" "$appdir/usr/bin/$f"
  strip --strip-unneeded "$appdir/usr/bin/$f"
done
# a build for older distros brings its C++ runtime, used only where the system's is older (AppRun)
if [[ -d $build/compat ]]; then
  mkdir -p "$appdir/usr/lib/compat"
  install -m 644 "$build"/compat/* "$appdir/usr/lib/compat/"
fi
# the shader translator for the native renderer: it turns the player's own disc's shaders into shaders.bin
# beside the AppImage on first use, so it ships, with the compiler it drives and their licences
xr="${BARDOWN_XENOSRECOMP:-$root/out/build/xenosrecomp}/XenosRecomp/XenosRecomp"
[[ -x $xr ]] || { echo "build the shader translator first (out/build/xenosrecomp, see docs/RENDERER.md)"; exit 1; }
install -m 755 "$xr" "$appdir/usr/bin/xenosrecomp"
strip --strip-unneeded "$appdir/usr/bin/xenosrecomp"
install -m 755 "$root/third_party/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$appdir/usr/bin/libdxcompiler.so"
install -m 644 "$root/third_party/XenosRecomp/XenosRecomp/shader_common.h" "$appdir/usr/bin/shader_common.h"
mkdir -p "$appdir/usr/share/licenses/bardown"
install -m 644 "$root"/packaging/licenses/*.txt "$appdir/usr/share/licenses/bardown/"
install -m 644 "$root/LICENSE" "$appdir/usr/share/licenses/bardown/bardown.txt"
install -m 755 "$root/packaging/AppRun" "$appdir/AppRun"
install -m 644 "$root/packaging/bardown.desktop" "$appdir/bardown.desktop"
install -m 644 "$root/packaging/bardown.desktop" "$appdir/usr/share/applications/bardown.desktop"
install -m 644 "$root/packaging/bardown.png" "$appdir/bardown.png"
install -m 644 "$root/packaging/bardown.png" "$appdir/usr/share/icons/hicolor/512x512/apps/bardown.png"
ln -sf bardown.png "$appdir/.DirIcon"

mkdir -p "$root/dist"
out="$root/dist/bardown-$version-x86_64.AppImage"
ARCH=x86_64 VERSION="$version" "$tool" --no-appstream "$appdir" "$out"
echo "$out"
