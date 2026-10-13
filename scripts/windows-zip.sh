#!/usr/bin/env bash
# builds dist/bardown-<version>-windows-x64.zip from the cross-compiled build (cmake/windows-xwin.cmake): the
# program, its runtime and graphics plugins, the shader translator with its compiler, and Microsoft's C++
# runtime, so it runs on a Windows 10 or 11 PC with nothing installed. unpacked anywhere, it keeps its
# settings, saves and logs in its own folder (or %LOCALAPPDATA%\bardown when that folder is read-only)
#   VC_REDIST=/path/to/vc_redist.x64.exe scripts/windows-zip.sh [version]
# vc_redist.x64.exe is Microsoft's own installer (https://aka.ms/vs/17/release/vc_redist.x64.exe); the
# runtime DLLs are taken out of it rather than installed
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
build="${BARDOWN_BUILD:-$root/out/build/win-cross}"
xr="${BARDOWN_XENOSRECOMP:-$root/out/build/xenosrecomp-win}/XenosRecomp"
version="${1:-$(git -C "$root" describe --tags --always --dirty)}"
redist="${VC_REDIST:?set VC_REDIST to vc_redist.x64.exe}"
[[ -f $build/bardown.exe ]] || { echo "no $build/bardown.exe: cross-compile first"; exit 1; }
[[ -f $xr/XenosRecomp.exe ]] || { echo "no $xr/XenosRecomp.exe: cross-compile the translator first"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
name="bardown-$version-windows-x64"
dir="$work/$name"
mkdir -p "$dir/licenses"

for f in bardown.exe rexruntime.dll rexgpu-native.dll rexgpu-xenos.dll; do
  install -m 755 "$build/$f" "$dir/$f"
done
# the translator under the name bardown looks for, with its compiler beside it
install -m 755 "$xr/XenosRecomp.exe" "$dir/xenosrecomp.exe"
install -m 755 "$xr/dxcompiler.dll" "$xr/dxil.dll" "$dir/"
install -m 644 "$root/third_party/XenosRecomp/XenosRecomp/shader_common.h" "$dir/shader_common.h"

# Microsoft's C++ runtime: the installer is a bundle with cabinets appended; the 64-bit DLLs sit in one of them
python3 -I - "$redist" "$work/redist" <<'EOF'
import os, struct, subprocess, sys
exe, out = sys.argv[1], sys.argv[2]
data = open(exe, 'rb').read()
os.makedirs(out, exist_ok=True)
at, n = 0, 0
while (at := data.find(b'MSCF\0\0\0\0', at)) >= 0:
    size = struct.unpack_from('<I', data, at + 8)[0]
    if 0 < size <= len(data) - at:
        path = f'{out}/outer{n}.cab'
        open(path, 'wb').write(data[at:at + size])
        subprocess.run(['cabextract', '-q', '-d', f'{out}/outer{n}', path], check=False, stderr=subprocess.DEVNULL)
        n += 1
    at += 4
for root, _, files in os.walk(out):
    for f in files:
        p = os.path.join(root, f)
        if open(p, 'rb').read(4) == b'MSCF':
            subprocess.run(['cabextract', '-q', '-d', f'{out}/inner', p], check=False, stderr=subprocess.DEVNULL)
EOF
for dll in msvcp140 msvcp140_atomic_wait vcruntime140 vcruntime140_1; do
  src="$(find "$work/redist/inner" -name "$dll.dll_amd64" | head -1)"
  [[ -n $src ]] || { echo "$dll.dll not found in $redist"; exit 1; }
  install -m 644 "$src" "$dir/$dll.dll"
done

install -m 644 "$root"/packaging/licenses/*.txt "$dir/licenses/"
install -m 644 "$root/LICENSE" "$dir/licenses/bardown.txt"
cat > "$dir/README.txt" <<EOF
bardown $version for Windows (10 or 11, 64-bit)

You need your own copy of NHL 14 for the Xbox 360: the base disc, no title update, as a disc image (.iso)
or an extracted folder. Nothing from the game comes with bardown.

1. Unpack this folder somewhere you can write to, such as C:\\Games\\bardown (not Program Files).
2. Put your NHL 14 disc image (.iso) in it.
3. Double-click bardown.exe. If Windows SmartScreen warns about an unknown app (the program isn't
   code-signed): More info, then Run anyway.

The first start takes a few minutes, once: it checks the disc, extracts it into game\\ and translates the
game's shaders for your GPU. After that it starts straight away, and the .iso can be deleted.

Settings, saves and logs stay in this folder. If it can't be written to, they go to %LOCALAPPDATA%\\bardown
instead (paste that into File Explorer's address bar).

Controls: any Xbox, PlayStation or Switch Pro controller. Without one, the keyboard: WASD left stick,
arrow keys right stick, Space A, Backspace B, L X, P Y, Q/E triggers, 1/3 bumpers, Enter Start, Tab Back.
F1 opens the settings (key binds included), F3 shows frame times.

If something goes wrong, the newest file in the logs folder says what happened. The GPU needs Vulkan 1.2
on a current driver; update the driver first if bardown says the GPU is missing something.
EOF

mkdir -p "$root/dist"
out="$root/dist/$name.zip"
rm -f "$out"
(cd "$work" && zip -qr9 "$out" "$name")
echo "$out"
