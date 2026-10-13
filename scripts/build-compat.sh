#!/usr/bin/env bash
# the Linux build for older distros, run inside packaging/Containerfile (Ubuntu 22.04, glibc 2.35). its own
# build and install folders, so the usual build is left alone; generated/ comes from a normal codegen run.
# libstdc++ 13 and libgcc_s go in compat/ beside it for the AppImage to fall back on: a distro with older ones
# gets these (linking them in statically breaks unwinding across the program's shared libraries).
# then on the host: BARDOWN_BUILD=out/build/linux-compat BARDOWN_XENOSRECOMP=out/build/xenosrecomp-compat \
#   scripts/appimage.sh <version>
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
jobs="${JOBS:-8}"
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS="-march=x86-64-v2 --gcc-toolchain=/usr"
        -DCMAKE_EXE_LINKER_FLAGS= -DCMAKE_SHARED_LINKER_FLAGS= -DCMAKE_MODULE_LINKER_FLAGS=)

sdk="$root/third_party/rexglue-sdk"
cmake -S "$sdk" -B "$sdk/out/build/linux-compat" "${common[@]}" -DCMAKE_INSTALL_PREFIX="$sdk/out/install/linux-compat"
cmake --build "$sdk/out/build/linux-compat" -j"$jobs" --target install

# xxHash's own option would make its dependencies shared libraries too; the translator ships as one file
cmake -S "$root/third_party/XenosRecomp" -B "$root/out/build/xenosrecomp-compat" "${common[@]}" -DBUILD_SHARED_LIBS=OFF
cmake --build "$root/out/build/xenosrecomp-compat" -j"$jobs"

cmake -S "$root" -B "$root/out/build/linux-compat" "${common[@]}" -DBARDOWN_SKIP_CODEGEN=ON \
  -Drexglue_DIR="$sdk/out/install/linux-compat/lib/cmake/rexglue"
cmake --build "$root/out/build/linux-compat" -j"$jobs"
# the runtime library goes beside the program, where the packaging and the AppImage's loader path expect it
cp -f "$sdk/out/install/linux-compat/lib/librexruntime.so" "$root/out/build/linux-compat/"
mkdir -p "$root/out/build/linux-compat/compat"
for lib in libstdc++.so.6 libgcc_s.so.1; do
  cp -fL "$(clang++ -print-file-name=$lib)" "$root/out/build/linux-compat/compat/$lib"
done
