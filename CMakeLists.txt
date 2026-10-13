# bardown - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bardown LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# cross-compiling (cmake/windows-xwin.cmake): the SDK's codegen tool is built for the target and can't run
# here. the generated code is the same for every target, so codegen is left to a native build and this one
# compiles what's already in generated/. BARDOWN_SKIP_CODEGEN does the same for a second native build
# (scripts/build-compat.sh), which would otherwise rewrite generated/ under the first
option(BARDOWN_SKIP_CODEGEN "Compile generated/ as it is, without running codegen" OFF)
if(CMAKE_CROSSCOMPILING OR BARDOWN_SKIP_CODEGEN)
  find_program(BARDOWN_NO_CODEGEN true REQUIRED)
  set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION ${BARDOWN_NO_CODEGEN}
                                                IMPORTED_LOCATION_RELEASE ${BARDOWN_NO_CODEGEN})
endif()

# Sources
set(BARDOWN_SOURCES
    src/main.cpp
    src/kernel_stubs.cpp
    src/load_watch.cpp
    src/anim_capture.cpp
    src/sound_capture.cpp
    src/match_capture.cpp
    src/speech_capture.cpp
    src/nis_capture.cpp
    src/presentation_capture.cpp
    src/storycam_capture.cpp
    src/overlay_capture.cpp
    src/controls_capture.cpp
    src/faceoff_capture.cpp
    src/moves_capture.cpp
    src/ai_capture.cpp
    src/carrier_capture.cpp
    src/gamecam_capture.cpp
    src/native/draw.cpp
    src/native/native.cpp
    src/native/pipelines.cpp
    src/native/render.cpp
    src/native/streams.cpp
    src/native/textures.cpp
)

if(WIN32)
    add_executable(bardown WIN32 ${BARDOWN_SOURCES})
else()
    add_executable(bardown ${BARDOWN_SOURCES})
endif()

# the SDK's headers include RenderDoc's api header, which the SDK doesn't install
target_include_directories(bardown PRIVATE ${CMAKE_SOURCE_DIR}/third_party/rexglue-sdk/thirdparty/renderdoc)
# the standard C calls (getenv, fopen, sscanf) are what's meant; Microsoft's CRT warns about each one
if(WIN32)
  target_compile_definitions(bardown PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

rexglue_setup_target(bardown GPU_PLUGINS xenos native)

# bardown's own code gets the full set of warnings (the generated code builds in its own library). clang-cl
# reads -Wall as -Weverything, so only the gcc-style drivers get them
if(NOT MSVC)
  set_source_files_properties(${BARDOWN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
  # vulkan structs are set up as {sType} and filled in by name
  set_source_files_properties(src/native/draw.cpp src/native/pipelines.cpp src/native/render.cpp src/native/streams.cpp src/native/textures.cpp
                              PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-missing-field-initializers")
endif()

# the overlay's font (Roboto, Apache 2.0, from imgui's font folder), built into the program
set(BARDOWN_FONT ${CMAKE_SOURCE_DIR}/third_party/rexglue-sdk/thirdparty/imgui/misc/fonts/Roboto-Medium.ttf)
set(BARDOWN_FONT_CPP ${CMAKE_BINARY_DIR}/overlay_font.cpp)
add_custom_command(
    OUTPUT ${BARDOWN_FONT_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${BARDOWN_FONT} -DOUT=${BARDOWN_FONT_CPP} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${BARDOWN_FONT} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
target_sources(bardown PRIVATE ${BARDOWN_FONT_CPP})
