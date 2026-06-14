#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "base.h"
#include "arena.h"

#define WIDTH  800
#define HEIGHT 600

#define PERMANENT_STORAGE_SIZE (4 * 1024 * 1024)

typedef struct {
    u32 *pixels;   // points at this view's top-left pixel
    int  width;    // logical drawable width
    int  height;   // logical drawable height
    int  stride;   // u32s per row in the underlying memory (parent width)
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
    bool is_initialized;
    Arena *persistent_memory;
    Arena *transient_memory;
} GameMemory;

typedef void (*game_update_and_render_t)(GameMemory *game_memory, GameInput *, GameAudio *, RenderBuffer *);
