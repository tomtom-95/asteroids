#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "base.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "game.h"

// ABGR packed for PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 on little-endian
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_GREEN 0xFF00FF00
#define COLOR_RED   0xFF0000FF

static double
random_number(int min, int max)
{
    return min + rand() % (max - min + 1);
}

static double
random_angle(double x, double y)
{
    return x + (double)rand() / ((double)RAND_MAX / (y - x));
}

static void
set_pixel(u32 *pixels, int width, int height, int x, int y, u32 color)
{
    int sx = (x + width)  % width;
    int sy = (y + height) % height;
    pixels[width * sy + sx] = color;
}

static void
draw_line_high(u32 *pixels, int width, int height, int x0, int y0, int x1, int y1, u32 color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int hstep = 1;
    if (dx < 0) { dx = -dx; hstep = -1; }
    int D = 2 * dx - dy;
    int x = x0;
    for (int y = y0; y < y1; ++y) {
        set_pixel(pixels, width, height, x, y, color);
        if (D > 0) { x += hstep; D += 2 * (dx - dy); }
        else        {             D += 2 * dx;          }
    }
}

static void
draw_line_low(u32 *pixels, int width, int height, int x0, int y0, int x1, int y1, u32 color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int vstep = 1;
    if (dy < 0) { dy = -dy; vstep = -1; }
    int D = 2 * dy - dx;
    int y = y0;
    for (int x = x0; x < x1; ++x) {
        set_pixel(pixels, width, height, x, y, color);
        if (D > 0) { y += vstep; D += 2 * (dy - dx); }
        else        {             D += 2 * dy;          }
    }
}

static void
draw_line(u32 *pixels, int width, int height, int x0, int y0, int x1, int y1, u32 color)
{
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) draw_line_low(pixels, width, height, x1, y1, x0, y0, color);
        else          draw_line_low(pixels, width, height, x0, y0, x1, y1, color);
    } else {
        if (y0 > y1) draw_line_high(pixels, width, height, x1, y1, x0, y0, color);
        else          draw_line_high(pixels, width, height, x0, y0, x1, y1, color);
    }
}

static void
draw_ship_at(u32 *pixels, int width, int height, int x_center, int y_center, float rotation, u32 color)
{
    float c = cosf(rotation);
    float s = sinf(rotation);

    int x0 = x_center + (int)(20*c);
    int y0 = y_center + (int)(20*s);
    int x1 = x_center + (int)(-10*c + 10*s);
    int y1 = y_center + (int)(-10*s - 10*c);
    int x2 = x_center + (int)(-10*c - 10*s);
    int y2 = y_center + (int)(-10*s + 10*c);

    set_pixel(pixels, width, height, x0, y0, color);
    draw_line(pixels, width, height, x0, y0, x1, y1, color);
    draw_line(pixels, width, height, x1, y1, x2, y2, color);
    draw_line(pixels, width, height, x2, y2, x0, y0, color);
}

static int
measure_text(GameState *state, const char *text, int font_size)
{
    if (!state->font_buffer) return 0;
    float scale = stbtt_ScaleForPixelHeight(&state->font, font_size);
    int total = 0;
    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&state->font, text[i], &advance, &lsb);
        total += (int)(advance * scale);
    }
    return total;
}

static void
draw_message(GameState *state, const char *text, int x, int y, int font_size)
{
    if (!state->font_buffer) return;

    float scale = stbtt_ScaleForPixelHeight(&state->font, font_size);
    int ascent;
    stbtt_GetFontVMetrics(&state->font, &ascent, NULL, NULL);

    int x_cursor   = x;
    int y_baseline = y + (int)(ascent * scale);

    for (int i = 0; text[i]; i++) {
        int w, h, xoff, yoff;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(
            &state->font, scale, scale, text[i], &w, &h, &xoff, &yoff);

        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                unsigned char alpha = bitmap[row * w + col];
                if (!alpha) continue;
                int px = x_cursor + xoff + col;
                int py = y_baseline + yoff + row;
                if (px >= 0 && px < state->width && py >= 0 && py < state->height)
                    state->pixels[state->width * py + px] = 0xFF000000 | ((u32)alpha << 16) | ((u32)alpha << 8) | alpha;
            }
        }
        stbtt_FreeBitmap(bitmap, NULL);

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&state->font, text[i], &advance, &lsb);
        x_cursor += (int)(advance * scale);
    }
}

static void
init_bullets(GameState *state)
{
    state->bullet_pool.first_used = 0;
    state->bullet_pool.first_free = 1;
    for (int i = 1; i < MAX_BULLET_COUNT; ++i)
        state->bullet_pool.bullets[i].next = (i + 1) % MAX_BULLET_COUNT;
}

static void
init_asteroids(GameState *state)
{
    state->asteroid_pool.first_used = 0;
    state->asteroid_pool.first_free = 1;
    for (int i = 1; i < MAX_ASTEROIDS_COUNT; ++i)
        state->asteroid_pool.asteroids[i].next = (i + 1) % MAX_ASTEROIDS_COUNT;
}

static int
bullet_alloc(BulletPool *pool)
{
    assert(pool->first_free != 0);
    int idx = pool->first_free;
    pool->first_free    = pool->bullets[idx].next;
    pool->bullets[idx].next = pool->first_used;
    pool->first_used    = idx;
    return idx;
}

static int
asteroid_alloc(AsteroidPool *pool)
{
    assert(pool->first_free != 0);
    int idx = pool->first_free;
    pool->first_free          = pool->asteroids[idx].next;
    pool->asteroids[idx].next = pool->first_used;
    pool->first_used          = idx;
    return idx;
}

static void
bullet_free(BulletPool *pool, int idx)
{
    Bullet *b = &pool->bullets[pool->first_used];
    if (pool->first_used == idx) {
        pool->first_used = b->next;
    } else {
        while (b->next != idx) b = &pool->bullets[b->next];
        b->next = pool->bullets[b->next].next;
    }
    pool->bullets[idx].next = pool->first_free;
    pool->first_free = idx;
}

static void
asteroid_free(AsteroidPool *pool, int idx)
{
    Asteroid *a = &pool->asteroids[pool->first_used];
    if (pool->first_used == idx) {
        pool->first_used = a->next;
    } else {
        while (a->next != idx) {
            a = &pool->asteroids[a->next];
        }
        a->next = pool->asteroids[a->next].next;
    }
    pool->asteroids[idx].next = pool->first_free;
    pool->first_free = idx;
}

static void
create_asteroids_at_startup(GameState *state)
{
    for (int i = 0; i < 20; ++i) {
        int idx = asteroid_alloc(&state->asteroid_pool);
        Asteroid *a = &state->asteroid_pool.asteroids[idx];

        a->x_center  = (int)random_number(0, state->width);
        a->y_center  = (int)random_number(0, state->height);
        a->radius    = ASTEROID_RADIUS;
        a->is_small  = false;
        a->x_velocity = ASTEROID_VELOCITY;
        a->y_velocity = ASTEROID_VELOCITY;

        double delta_angle = (2.0 * PI) / ASTEROID_VERTICES;
        for (int j = 0; j < ASTEROID_VERTICES; ++j) {
            double angle = random_angle(j * delta_angle, (j + 1) * delta_angle);
            a->x_vertices[j] = a->x_center + (int)(a->radius * cosf(angle));
            a->y_vertices[j] = a->y_center + (int)(a->radius * sinf(angle));
        }
    }
}

static void
game_init(GameState *state, double current_time)
{
    state->x_center    = state->width  / 2;
    state->y_center    = state->height / 2;
    state->last_time_hit = current_time - INVISIBILITY_TIME;
    state->ship_color  = COLOR_WHITE;

    FILE *f = fopen("resources/PressStart2P-Regular.ttf", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        state->font_buffer = malloc(size);
        fread(state->font_buffer, 1, size, f);
        fclose(f);
        stbtt_InitFont(&state->font, state->font_buffer, 0);
    }

    init_asteroids(state);
    init_bullets(state);
    create_asteroids_at_startup(state);

    state->is_initialized = true;
}

static void
draw_asteroids(GameState *state)
{
    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a = &state->asteroid_pool.asteroids[idx];
        int x0 = a->x_vertices[0];
        int y0 = a->y_vertices[0];
        for (int j = 1; j < ASTEROID_VERTICES + 1; ++j) {
            int x1 = a->x_vertices[j % ASTEROID_VERTICES];
            int y1 = a->y_vertices[j % ASTEROID_VERTICES];
            draw_line(state->pixels, state->width, state->height, x0, y0, x1, y1, COLOR_WHITE);
            x0 = x1; y0 = y1;
        }
        idx = a->next;
    }
}

static void
draw_bullets(GameState *state)
{
    int idx = state->bullet_pool.first_used;
    while (idx) {
        Bullet *b = &state->bullet_pool.bullets[idx];
        set_pixel(state->pixels, state->width, state->height, b->bullet_x, b->bullet_y, COLOR_WHITE);
        idx = b->next;
    }
}

static void
update_ship_velocity(GameState *state, GameInput *input)
{
    if (input->left)  state->x_velocity = Max(state->x_velocity - VELOCITY_UPDATE, -MAX_VELOCITY);
    if (input->right) state->x_velocity = Min(state->x_velocity + VELOCITY_UPDATE,  MAX_VELOCITY);
    if (input->up)    state->y_velocity = Max(state->y_velocity - VELOCITY_UPDATE, -MAX_VELOCITY);
    if (input->down)  state->y_velocity = Min(state->y_velocity + VELOCITY_UPDATE,  MAX_VELOCITY);

    if (state->x_velocity > 0)
        state->x_velocity = (state->x_velocity > DRAG_UPDATE) ? state->x_velocity - DRAG_UPDATE : 0;
    else if (state->x_velocity < 0)
        state->x_velocity = (state->x_velocity < -DRAG_UPDATE) ? state->x_velocity + DRAG_UPDATE : 0;

    if (state->y_velocity > 0)
        state->y_velocity = (state->y_velocity > DRAG_UPDATE) ? state->y_velocity - DRAG_UPDATE : 0;
    else if (state->y_velocity < 0)
        state->y_velocity = (state->y_velocity < -DRAG_UPDATE) ? state->y_velocity + DRAG_UPDATE : 0;
}

static void
update_ship_position(GameState *state, double dt)
{
    state->x_center = (state->x_center + (int)(state->x_velocity * dt) + state->width)  % state->width;
    state->y_center = (state->y_center + (int)(state->y_velocity * dt) + state->height) % state->height;
}

static void
update_ship_rotation(GameState *state, double dt)
{
    if (state->x_velocity == 0 && state->y_velocity == 0) return;

    float target = atan2f(state->y_velocity, state->x_velocity);
    float diff   = target - state->rotation;
    while (diff >  PI) diff -= 2*PI;
    while (diff < -PI) diff += 2*PI;

    float step = 6.0f * (float)dt;
    if      (fabsf(diff) < step) state->rotation = target;
    else if (diff > 0)           state->rotation += step;
    else                         state->rotation -= step;
}

static void
update_asteroids_positions(GameState *state, double dt)
{
    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a = &state->asteroid_pool.asteroids[idx];
        int dx = (int)(a->x_velocity * dt);
        int dy = (int)(a->y_velocity * dt);
        a->x_center = (a->x_center + dx + state->width)  % state->width;
        a->y_center = (a->y_center + dy + state->height) % state->height;
        for (int j = 0; j < ASTEROID_VERTICES; ++j) {
            a->x_vertices[j] += dx;
            a->y_vertices[j] += dy;
        }
        idx = a->next;
    }
}

static void
update_bullets_positions(GameState *state, double dt)
{
    int idx = state->bullet_pool.first_used;
    while (idx) {
        Bullet *b = &state->bullet_pool.bullets[idx];
        b->bullet_x += (int)(b->bullet_x_velocity * dt);
        b->bullet_y += (int)(b->bullet_y_velocity * dt);
        idx = b->next;
    }
}

static void
shoot_bullet_from_ship(GameState *state)
{
    int idx = bullet_alloc(&state->bullet_pool);
    Bullet *b = &state->bullet_pool.bullets[idx];
    b->bullet_x = state->x_center;
    b->bullet_y = state->y_center;
    b->bullet_x_velocity = BULLET_VELOCITY * cosf(state->rotation);
    b->bullet_y_velocity = BULLET_VELOCITY * sinf(state->rotation);
}

static void
free_out_of_window_bullets(GameState *state)
{
    int idx = state->bullet_pool.first_used;
    while (idx) {
        Bullet *b  = &state->bullet_pool.bullets[idx];
        int    next = b->next;
        bool in_bound = (int)b->bullet_x > 0 && (int)b->bullet_x < state->width
                     && (int)b->bullet_y > 0 && (int)b->bullet_y < state->height;
        if (!in_bound) bullet_free(&state->bullet_pool, idx);
        idx = next;
    }
}

static void
divide_asteroid(GameState *state, int x, int y)
{
    double delta_angle = (2.0 * PI) / ASTEROID_VERTICES;

    for (int half = 0; half < 2; ++half) {
        int idx = asteroid_alloc(&state->asteroid_pool);
        Asteroid *a = &state->asteroid_pool.asteroids[idx];
        a->x_center = x;
        a->y_center = y;
        a->radius   = ASTEROID_RADIUS / 2;
        a->is_small = true;

        double angle = random_angle(half * PI, (half + 1) * PI);
        a->x_velocity = ASTEROID_VELOCITY * cosf(angle);
        a->y_velocity = ASTEROID_VELOCITY * sinf(angle);

        for (int j = 0; j < ASTEROID_VERTICES; ++j) {
            double va = random_angle(j * delta_angle, (j + 1) * delta_angle);
            a->x_vertices[j] = x + (int)(a->radius * cosf(va));
            a->y_vertices[j] = y + (int)(a->radius * sinf(va));
        }
    }
}

static void
check_asteroid_bullet_collision(GameState *state)
{
    int a_idx = state->asteroid_pool.first_used;
    while (a_idx) {
        Asteroid *a      = &state->asteroid_pool.asteroids[a_idx];
        int       a_next = a->next;
        int       ax     = a->x_center;
        int       ay     = a->y_center;
        bool      small  = a->is_small;

        int b_idx = state->bullet_pool.first_used;
        while (b_idx) {
            Bullet *b      = &state->bullet_pool.bullets[b_idx];
            int     b_next = b->next;

            int dx = abs(ax - (int)b->bullet_x);
            int dy = abs(ay - (int)b->bullet_y);
            int d  = (int)sqrtf((float)(Square(dx) + Square(dy)));

            if (d < ASTEROID_RADIUS) {
                state->score += 1;
                bullet_free(&state->bullet_pool, b_idx);
                asteroid_free(&state->asteroid_pool, a_idx);
                if (!small) divide_asteroid(state, ax, ay);
                break;
            }
            b_idx = b_next;
        }
        a_idx = a_next;
    }
}

static void
check_asteroid_ship_collision(GameState *state, double current_time)
{
    if (current_time - state->last_time_hit <= INVISIBILITY_TIME) return;

    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a = &state->asteroid_pool.asteroids[idx];
        int dx = abs(a->x_center - state->x_center);
        int dy = abs(a->y_center - state->y_center);
        int d  = (int)sqrtf((float)(Square(dx) + Square(dy)));
        if (d < ASTEROID_RADIUS + 15) {
            state->hitted   = true;
            state->x_center = state->width  / 2;
            state->y_center = state->height / 2;
        }
        idx = a->next;
    }
}

static void
check_invisibility(GameState *state, double current_time)
{
    double delta = current_time - state->last_time_hit;
    if (delta < INVISIBILITY_TIME)
        state->ship_color = ((int)(4 * delta) % 2 == 0) ? COLOR_GREEN : COLOR_RED;
    else
        state->ship_color = COLOR_WHITE;
}

void
game_update_and_render(GameState *state, GameInput *input, GameAudio *audio)
{
    if (state->state_size != sizeof(GameState)) {
        u32 *pixels = state->pixels;
        memset(state, 0, sizeof(GameState));
        state->pixels      = pixels;
        state->width       = WIDTH;
        state->height      = HEIGHT;
        state->state_size  = sizeof(GameState);
    }

    if (!state->is_initialized) game_init(state, input->current_time);

    audio->play_hit_sound = false;

    double dt = input->dt;

    update_ship_velocity(state, input);
    update_ship_position(state, dt);
    update_ship_rotation(state, dt);

    if (input->bullet_shot) shoot_bullet_from_ship(state);

    update_asteroids_positions(state, dt);
    update_bullets_positions(state, dt);
    free_out_of_window_bullets(state);

    check_asteroid_ship_collision(state, input->current_time);
    if (state->hitted) {
        audio->play_hit_sound  = true;
        state->last_time_hit   = input->current_time;
        state->hitted          = false;
    }

    check_invisibility(state, input->current_time);
    check_asteroid_bullet_collision(state);

    memset(state->pixels, 0, 4 * state->width * state->height);

    draw_ship_at(state->pixels, state->width, state->height,
                 state->x_center, state->y_center, state->rotation, state->ship_color);
    draw_asteroids(state);
    draw_bullets(state);

    char str_score[20];
    snprintf(str_score, sizeof(str_score), "%d", state->score);
    int w = measure_text(state, str_score, 30);
    draw_message(state, str_score, state->width - w - 10, 10, 30);
}
