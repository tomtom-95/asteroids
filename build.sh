#!/bin/sh
set -e

mkdir -p build

FLAGS="-I/opt/homebrew/include -Wall -Wno-deprecated-declarations -g -O0"

build_game()     { clang -dynamiclib -o build/game.dylib game.c $FLAGS; }
build_platform() { clang -o build/asteroids platform.c $FLAGS -L/opt/homebrew/lib -lraylib; }

case "${1:-all}" in
    game) build_game ;;
    all)  build_game && build_platform ;;
    *)    echo "Usage: $0 [game|all]"; exit 1 ;;
esac
