#!/bin/sh
# Instruction-semantics regression test for runtime/nvn/maxwell/translate.c (see tools/test_translate_ops.c):
# translates small programs, runs their MSL bodies on the CPU and checks the results - then their GLSL bodies, with
# 64-bit integers and without (maxwell_no_int64).
# usage: tools/test_translate_ops.sh   (needs clang / clang++)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_translate_ops
mkdir -p "$OUT"
CC=${CC:-clang}
CXX=${CXX:-clang++}
$CC -O1 -Wall tools/test_translate_ops.c runtime/nvn/maxwell/translate.c -o "$OUT/gen" -lm
for mode in msl "glsl" "glsl 32"; do
    name=$(echo "$mode" | tr -d ' ')
    "$OUT/gen" "$OUT/cases_$name.cpp" $([ "$mode" = msl ] || echo "$mode")
    $CXX -std=c++17 -O1 -w "$OUT/cases_$name.cpp" -o "$OUT/cases_$name"
    printf '%s: ' "$name"
    "$OUT/cases_$name"
done
