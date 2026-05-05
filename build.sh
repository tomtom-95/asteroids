#!/bin/sh
set -e

mkdir -p build

TARGET=${1:-all}

if [ "$TARGET" = "game" ] || [ "$TARGET" = "all" ]; then
    clang -dynamiclib -o build/game.dylib game.c \
        -I/opt/homebrew/include \
        -Wall -Wno-deprecated-declarations
fi

if [ "$TARGET" = "platform" ] || [ "$TARGET" = "all" ]; then
    clang -o build/asteroids platform.c \
        -I/opt/homebrew/include \
        -L/opt/homebrew/lib -lraylib \
        -Wall -Wno-deprecated-declarations
fi
