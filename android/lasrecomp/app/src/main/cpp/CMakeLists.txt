# libmain.so: the LAS Recomp runtime for Android (what the Makefile builds into the desktop executable), with the
# Vulkan backend and the ImGui UI. The game code is not in it: lasgame.so, generated with gen.py --dyn-base and built
# from the player's own game files, is loaded at run time (docs/ANDROID-PORT.md).
cmake_minimum_required(VERSION 3.22)
project(lasrecomp C CXX ASM)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)   # the repository
get_filename_component(R ${R} ABSOLUTE)
set(DEPS ${R}/android/deps/out)                        # android/deps/build_deps.sh
if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "run android/deps/build_deps.sh first")
endif()
if(NOT LAS_GEN_DIR)
    message(FATAL_ERROR "LAS_GEN_DIR (the Gradle build's generated files: smaa_hlsl.inc) is not set")
endif()

include(${R}/android/gamelib_id.cmake)  # (GAMELIB_ID: the same as lasgame.so's, android/gamelib)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

# Shader preparation (runtime/nvn/shader_prep.c): the variant list as a C string, and the shader translator's id - part
# of the tag that says whether the shader caches are cold (a changed translator, its GLSL rewrites or pipeline keys
# prepare again)
file(STRINGS ${R}/runtime/nvn/shader_variants.txt _variants)
set(_inc "")
foreach(l IN LISTS _variants)
    string(REPLACE "\r" "" l "${l}")  # (a Windows checkout's line ends)
    string(APPEND _inc "\"${l}\\n\"\n")
endforeach()
string(APPEND _inc "\"\"\n")
file(WRITE ${CMAKE_BINARY_DIR}/inc/shader_variants.inc "${_inc}")
set(_tr "")
foreach(f runtime/nvn/shader_variants.txt runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
          runtime/nvn/vulkan/exec.c runtime/nvn/vulkan/shaders.c)
    file(SHA256 ${R}/${f} _h)
    string(APPEND _tr ${_h})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/${f})
endforeach()
string(SHA256 _tr "${_tr}")
string(SUBSTRING ${_tr} 0 12 SHADER_TRANSLATOR_ID)
message(STATUS "SHADER_TRANSLATOR_ID ${SHADER_TRANSLATOR_ID}")

add_subdirectory(${R}/third_party/libadrenotools ${CMAKE_BINARY_DIR}/adrenotools)

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libSDL3.so)
add_library(shaderc_shared SHARED IMPORTED)
set_target_properties(shaderc_shared PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libshaderc_shared.so)

file(GLOB RT_SRC CONFIGURE_DEPENDS ${R}/runtime/*.c ${R}/runtime/hle/*.c ${R}/runtime/services/*.c ${R}/runtime/nvn/*.c
     ${R}/runtime/nvn/maxwell/*.c)
# FFmpeg (android/deps/build_ffmpeg.sh): the game's movies and the start screen's background; without it, neither
set(HAVE_FFMPEG FALSE)
if(EXISTS ${DEPS}/lib/libavformat.so)
    set(HAVE_FFMPEG TRUE)
    foreach(l avcodec avformat avutil swscale)
        add_library(${l} SHARED IMPORTED)
        set_target_properties(${l} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${l}.so)
    endforeach()
else()
    list(REMOVE_ITEM RT_SRC ${R}/runtime/hle/movie.c)
endif()
file(GLOB VK_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${R}/runtime/ui/*.mm)
list(REMOVE_ITEM UI_SRC ${R}/runtime/ui/rando_tab.mm)  # (no randomizer on Android: menu.mm leaves its tab out)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

add_library(main SHARED ${RT_SRC} ${R}/runtime/platform/android.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC}
            ${DEPS}/volk/volk.c ${R}/android/common/guest_window.c ${R}/runtime/native_gate.S)
# the UI sources are plain C++ apart from __APPLE__ blocks (as on Linux / Windows)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/nvn/vulkan/exec.c PROPERTIES
                            COMPILE_DEFINITIONS "SHADER_TRANSLATOR_ID=\"${SHADER_TRANSLATOR_ID}\"")

target_include_directories(main PRIVATE ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${DEPS}/include
                           ${DEPS}/volk ${LAS_GEN_DIR} ${R}/third_party/smaa/Textures ${R}/android/common
                           ${CMAKE_BINARY_DIR}/inc)
target_compile_definitions(main PRIVATE
    VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR IMGUI_IMPL_VULKAN_USE_VOLK   # Vulkan from the loaded driver (volk)
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
if(LAS_APP_VERSION)  # (the app's versionName, from build.gradle.kts: the start screen's lower right)
    target_compile_definitions(main PRIVATE LAS_APP_VERSION="${LAS_APP_VERSION}")
endif()
if(HAVE_FFMPEG)
    target_link_libraries(main PRIVATE avcodec avformat avutil swscale)
else()
    target_compile_definitions(main PRIVATE LARECOMP_NO_FFMPEG)
endif()
# -O2 -g whatever the Gradle build type (the Makefile's RTFLAGS): a debug build left the runtime at -O0, about half of the
# game thread's time
target_compile_options(main PRIVATE -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format  # (as on Linux: %llx for uint64_t)
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_link_libraries(main PRIVATE SDL3 shaderc_shared adrenotools android log dl m)
# (runtime/hle.h, ipc.h, main.c: tables in named sections found through __start_ / __stop_ symbols)
target_link_options(main PRIVATE -Wl,-z,nostart-stop-gc)
