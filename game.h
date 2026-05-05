#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "base.h"

#define WIDTH  800
#define HEIGHT 600

#define PERMANENT_STORAGE_SIZE (4 * 1024 * 1024)

typedef struct {
    u32 *pixels;
    int  width;
    int  height;
} RenderBuffer;

typedef struct {
    int    left, right, up, down;
    bool   bullet_shot;
    double current_time;
    double dt;
} GameInput;

typedef struct {
    bool play_hit_sound;
} GameAudio;

typedef struct {
    size_t permanent_storage_size;
    void  *permanent_storage;
} GameMemory;

typedef void (*game_update_and_render_t)(GameMemory *, GameInput *, GameAudio *, RenderBuffer *);
