# lasgame.so for Android: the game code generated from the player's own game files with gen.py --dyn-base
# (android/build_gamelib.sh runs the generator and this build). The runtime functions it calls are libmain.so's: it
# names libmain.so as a dependency, which is how Android's linker finds them when the runtime loads it (dlopen).
#   LAS_GEN: the generated code (sdk/, main/)   LAS_LIBMAIN: the app's libmain.so (to link against)
cmake_minimum_required(VERSION 3.22)
project(lasgame C)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(R ${R} ABSOLUTE)
include(${R}/android/gamelib_id.cmake)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

file(GLOB GEN_SRC ${LAS_GEN}/main/*.c ${LAS_GEN}/sdk/*.c)
list(LENGTH GEN_SRC n)
if(n EQUAL 0)
    message(FATAL_ERROR "no generated code in ${LAS_GEN}")
endif()
# the library's own id adds the game's main build ID (runtime/gamelib.h gamelib_full_id): gen.py wrote it next to main
if(NOT EXISTS ${LAS_GEN}/main/main_build_id.txt)
    message(FATAL_ERROR "no ${LAS_GEN}/main/main_build_id.txt: generate main again (tools/gen.py)")
endif()
file(READ ${LAS_GEN}/main/main_build_id.txt _build_id)
string(SUBSTRING "${_build_id}" 0 16 _build_id)
set(GAMELIB_FULL_ID "${GAMELIB_ID}-${_build_id}")
if(NOT EXISTS ${LAS_LIBMAIN})
    message(FATAL_ERROR "LAS_LIBMAIN (the app's libmain.so) not found: ${LAS_LIBMAIN}")
endif()

add_library(main SHARED IMPORTED)
set_target_properties(main PROPERTIES IMPORTED_LOCATION ${LAS_LIBMAIN} IMPORTED_NO_SONAME FALSE)

add_library(lasgame SHARED ${GEN_SRC} ${R}/tools/gamelib_entry.c)
target_include_directories(lasgame PRIVATE ${R}/runtime)
target_compile_options(lasgame PRIVATE ${GAMELIB_FLAGS})
set_source_files_properties(${R}/tools/gamelib_entry.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_FULL_ID}\"")
target_link_libraries(lasgame PRIVATE main)
set_target_properties(lasgame PROPERTIES PREFIX "")   # lasgame.so (runtime/gamelib.h GAMELIB_NAME)
file(WRITE ${CMAKE_BINARY_DIR}/lasgame.so.id "${GAMELIB_FULL_ID}\n")
