#!/usr/bin/env bash
# rebuilds the native renderer's own shaders (src/native/shaders/*.vert|frag -> *.h). needs glslc
set -euo pipefail
cd "$(dirname "$0")/../src/native/shaders"
for f in *.vert *.frag; do
  out="${f%.*}_${f##*.}.h"
  name="k_${f%.*}_${f##*.}"
  tmp=$(mktemp)
  glslc -O --target-env=vulkan1.2 -mfmt=num -o "$tmp" "$f"
  {
    echo "// built from $f by tools/native_shaders.sh"
    echo "#pragma once"
    echo "#include <cstdint>"
    echo "inline constexpr uint32_t ${name}[] = {"
    cat "$tmp"
    echo "};"
  } > "$out"
  rm -f "$tmp"
done
