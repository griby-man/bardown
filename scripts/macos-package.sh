#!/usr/bin/env bash
# the universal macOS app and its .dmg, from the two per-architecture builds the macos workflow makes
# (mac-arm64.tar and mac-x86_64.tar in the current folder). runs on a Mac: lipo, sips, iconutil, codesign, hdiutil
#   scripts/macos-package.sh <version>    -> bardown-<version>-macos-universal.dmg
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?version}"
tar -xf mac-arm64.tar
tar -xf mac-x86_64.tar
app=dist/bardown.app
bin="$app/Contents/MacOS"
rm -rf dist
mkdir -p "$bin" "$app/Contents/Resources"

# each program and library joined into one universal file; ones that already are (the shader compiler) and plain
# files are copied as they are
(cd mac-arm64 && find . -type f) | while read -r f; do
  a="mac-arm64/$f" x="mac-x86_64/$f" o="$bin/$f"
  mkdir -p "$(dirname "$o")"
  if [ -f "$x" ] && file "$a" | grep -q Mach-O && [ "$(lipo -archs "$a")" != "$(lipo -archs "$x")" ] \
     && ! lipo -archs "$a" | grep -q ' '; then
    lipo -create "$a" "$x" -output "$o"
  else
    cp "$a" "$o"
  fi
done
(cd mac-arm64 && find . -type l) | while read -r f; do cp -P "mac-arm64/$f" "$bin/$f"; done

# Contents/MacOS holds only programs and libraries, or the bundle can't be signed: the Vulkan loader, MoltenVK and
# its driver file go in Resources/vulkan (where the runtime looks inside an app; the file's ../../../lib path still
# holds), and the translator's header goes in Resources (where the app looks for it too)
mv "$bin/vulkan" "$app/Contents/Resources/vulkan"
mv "$bin/shader_common.h" "$app/Contents/Resources/"
chmod +x "$bin/bardown" "$bin/xenosrecomp"
lipo -info "$bin/bardown" "$bin/librexruntime.dylib" "$bin/xenosrecomp"

mkdir -p icon.iconset
for s in 16 32 128 256 512; do
  sips -z $s $s "$root/packaging/bardown.png" --out icon.iconset/icon_${s}x${s}.png > /dev/null
  sips -z $((s*2)) $((s*2)) "$root/packaging/bardown.png" --out icon.iconset/icon_${s}x${s}@2x.png > /dev/null
done
iconutil -c icns icon.iconset -o "$app/Contents/Resources/bardown.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>bardown</string>
  <key>CFBundleExecutable</key><string>bardown</string>
  <key>CFBundleIdentifier</key><string>org.enoki.bardown</string>
  <key>CFBundleIconFile</key><string>bardown</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>LSMinimumSystemVersion</key><string>13.3</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

# an ad-hoc signature: not notarised, but Apple Silicon won't run an unsigned arm64 binary at all. every library and
# helper program first, then the app, which signs the main program and checks all of the rest (--deep doesn't
# reach code kept in Resources, and signing the main program on its own already wants everything beside it signed)
find "$app/Contents" -type f ! -path "$bin/bardown" | while read -r f; do
  if file "$f" | grep -q Mach-O; then codesign --force --sign - "$f"; fi
done
codesign --force --sign - "$app"
codesign --verify --strict --verbose=2 "$app"

ln -s /Applications dist/Applications
cp "$root"/packaging/licenses/*.txt dist/ 2>/dev/null || true
hdiutil create -volname bardown -srcfolder dist -ov -format UDZO "bardown-$version-macos-universal.dmg"
echo "bardown-$version-macos-universal.dmg"
