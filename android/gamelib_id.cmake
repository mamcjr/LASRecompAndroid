# GAMELIB_ID for Android (runtime/gamelib.h): what the game code is built from, as the Makefile computes it (the
# generator, the headers the generated code is compiled against, and its flags). Included by the app's CMakeLists.txt
# (libmain.so checks it) and android/gamelib/CMakeLists.txt (lasgame.so carries it), so the two always agree.
# Needs R (the repository). Sets GAMELIB_ID and GAMELIB_FLAGS (the generated code's compile flags, as a list).
# -O2 (the Makefile has -O1, for players who build it on their own machine; this one is built on the developer's):
set(GAMELIB_FLAGS -O2 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC)
# (as the Makefile's GAMELIB_SRCS: the game versions and their patch tables first)
file(GLOB _gamelib_tables RELATIVE ${R} ${R}/runtime/game_addresses/patches_v*.h)
list(SORT _gamelib_tables)
set(_gamelib_srcs tools/game_addresses.py runtime/game_addresses/versions.h ${_gamelib_tables}
    tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py
    tools/simd_c.py tools/rando/patch_variants.json tools/gamelib_entry.c runtime/hle/hle_symbols.txt
    runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h)
set(_id_text "")
foreach(f ${_gamelib_srcs})
    file(READ ${R}/${f} _t)
    string(APPEND _id_text "${_t}")
    # (a change re-runs the configure step: the id was only computed then, so a changed tools/patches.py alone left the
    # old id - the app kept the old game code, without the new patches)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/${f})
endforeach()
string(SHA256 _id "${_id_text}${GAMELIB_FLAGS} --simd asm --dyn-base")
string(SUBSTRING ${_id} 0 16 GAMELIB_ID)
