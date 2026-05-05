#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "base.h"
#include "stb_truetype.h"

#define WIDTH  800
#define HEIGHT 600

#define VELOCITY_UPDATE              200.0f
#define MAX_VELOCITY                 500.0f
#define DRAG_UPDATE                  100.0f
#define ASTEROID_VELOCITY            200.0f
#define ASTEROID_VELOCITY_RANDOMNESS 50
#define ASTEROID_RADIUS              20
#define MAX_ASTEROIDS_COUNT          100
#define ASTEROID_VERTICES            4
#define INVISIBILITY_TIME            2.0
#define BULLET_SHOT_TIME             0.5

#define MAX_BULLET_COUNT 100
#define BULLET_VELOCITY  600.0f

typedef struct {
    int   next;
    float x_velocity, y_velocity;
    int   x_center, y_center;
    int   radius;
    int   x_vertices[ASTEROID_VERTICES];
    int   y_vertices[ASTEROID_VERTICES];
    bool  is_small;
} Asteroid;

typedef struct {
    int   next;
    float bullet_x_velocity, bullet_y_velocity;
    u32   bullet_x, bullet_y;
} Bullet;

typedef struct {
    int    first_free, first_used;
    Bullet bullets[MAX_BULLET_COUNT];
} BulletPool;

typedef struct {
    int      first_free, first_used;
    Asteroid asteroids[MAX_ASTEROIDS_COUNT];
} AsteroidPool;

typedef struct {
    int    left, right, up, down;
    bool   bullet_shot;
    double current_time;
    double dt;
} GameInput;

typedef struct {
    bool play_hit_sound;
} GameAudio;

#define GAME_STATE_MAX_SIZE (4 * 1024 * 1024)

typedef struct {
    size_t state_size;     // must be first — used to detect layout changes on hot reload
    bool   is_initialized;

    u32 *pixels;
    int  width, height;

    double last_time_hit;

    float x_velocity, y_velocity;
    int   x_center, y_center;
    float rotation;
    u32   ship_color;

    BulletPool   bullet_pool;
    AsteroidPool asteroid_pool;

    bool hitted;
    int  score;

    stbtt_fontinfo font;
    unsigned char *font_buffer;
} GameState;

typedef void (*game_update_and_render_t)(GameState *, GameInput *, GameAudio *);
