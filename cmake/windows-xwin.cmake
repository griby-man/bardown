# cross-compiling for 64-bit Windows from Linux with clang and lld, against Microsoft's CRT and Windows SDK
# as unpacked by xwin (`xwin --accept-license splat --output ~/.xwin`). XWIN_DIR points elsewhere if needed.
#   cmake -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/windows-xwin.cmake ...

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_DIR)
  set(XWIN_DIR "$ENV{HOME}/.xwin")
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin's splat output: crt/ and sdk/")
# try_compile projects get the toolchain file again, without the cache
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET ${triple})
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_LINKER lld-link)

set(xwin_includes
    "${XWIN_DIR}/crt/include"
    "${XWIN_DIR}/sdk/include/ucrt"
    "${XWIN_DIR}/sdk/include/um"
    "${XWIN_DIR}/sdk/include/shared"
    "${XWIN_DIR}/sdk/include/winrt")
# the same CPU level as the other builds (passing CMAKE_<LANG>_FLAGS would replace these, headers and all)
set(xwin_flags "-march=x86-64-v2")
# what clang-cl's -imsvc does: system headers after clang's own (its intrinsics have to win over MSVC's)
foreach(dir IN LISTS xwin_includes)
  string(APPEND xwin_flags " -Xclang -internal-isystem -Xclang \"${dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${xwin_flags}")
set(CMAKE_CXX_FLAGS_INIT "${xwin_flags}")
set(CMAKE_RC_FLAGS_INIT "-I \"${XWIN_DIR}/sdk/include/um\" -I \"${XWIN_DIR}/sdk/include/shared\"")

set(xwin_link "-fuse-ld=lld")
foreach(dir crt/lib/x86_64 sdk/lib/ucrt/x86_64 sdk/lib/um/x86_64)
  string(APPEND xwin_link " -L\"${XWIN_DIR}/${dir}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${xwin_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${xwin_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${xwin_link}")

# xwin brings the release CRT only, so everything (compiler checks included) links against it
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
