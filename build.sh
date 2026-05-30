#!/bin/sh
set -e

mkdir -p build

FLAGS="-I/opt/homebrew/include -Wall -Wno-deprecated-declarations -g -O0"

build_game()     { clang -dynamiclib -o build/game.dylib game.c $FLAGS -lcurl; }
build_platform() { clang -o build/asteroids platform.c $FLAGS -L/opt/homebrew/lib -lraylib; }
build_test()     { clang -o build/test test_arena.c $FLAGS; }

case "${1:-all}" in
    game) build_game ;;
    all)  build_game && build_platform ;;
    test) build_test ;;
    *)    echo "Usage: $0 [game|all|test]"; exit 1 ;;
esac
