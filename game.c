#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <curl/curl.h>

#include "base.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "game.h"

// ── Constants ────────────────────────────────────────────────────────────────

#define VELOCITY_UPDATE   200.0f
#define MAX_VELOCITY      500.0f
#define DRAG_UPDATE       100.0f
#define ASTEROID_VELOCITY 100.0f
#define ASTEROID_RADIUS   10
#define ASTEROID_VERTICES 8
#define INVISIBILITY_TIME 1.0

#define MESSAGE_TIME 5

#define MAX_ASTEROIDS_COUNT 100
#define MAX_BULLET_COUNT    100
#define BULLET_VELOCITY     600.0f

// ABGR packed for PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 on little-endian
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_GREEN 0xFF00FF00
#define COLOR_RED   0xFF0000FF


// ── Private types ─────────────────────────────────────────────────────────────

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
    size_t state_size;  // first field — detects layout changes on hot reload

    double last_time_hit;
    double last_time_message_changed;
    float  x_velocity, y_velocity;
    int    x_center, y_center;
    float  rotation;
    u32    ship_color;

    BulletPool   bullet_pool;
    AsteroidPool asteroid_pool;

    bool hitted;
    int  score;

    stbtt_fontinfo font;
    unsigned char *font_buffer;

    stbtt_fontinfo font_comic_sans;
    unsigned char *font_buffer_comic_sans;

    u32 knowledge_base_size;
    u8 *knowledge_base;
    u8 *message;
} GameState;

char npc_response[2048] = "Press [E] to speak to the wizard...";

// Struct to help libcurl store the response data dynamically
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for libcurl to write incoming data chunks
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Simple quick-and-dirty helper to extract the "response" field from Ollama's JSON
// For complex games, consider using a proper library like cJSON.
void ExtractOllamaResponse(const char* json_str, char* dest, size_t dest_max) {
    const char* key = "\"response\":\"";
    const char* start = strstr(json_str, key);

    
    if (start) {
        start += strlen(key);
        size_t i = 0;
        
        // Copy until the closing unescaped quote (ignoring simple \" for now)
        while (*start && *start != '"' && i < dest_max - 1) {
            // Handle simple escape characters if necessary
            if (*start == '\\' && *(start + 1) == 'n') {
                dest[i++] = '\n';
                start += 2;
            } else {
                dest[i++] = *start;
                start++;
            }
        }
        dest[i] = '\0';
    }
    else {
        snprintf(dest, dest_max, "Error: Could not parse response.");
    }
}

// The background worker function
void* FetchLLMDialogue(void* arg)
{
    // TODO: instead of just passing the prompt.json, the payload must be enhanced using RAG

    // GameState *game_state = (GameState *)arg;
    // const char* player_prompt = (const char*)arg;

    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);  // Will be grown as needed by the realloc above
    chunk.size = 0;    

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if(curl_handle)
    {
        curl_easy_setopt(curl_handle, CURLOPT_URL, "http://localhost:11434/api/generate");

        char *payload = 0;
        long length;
        FILE *f = fopen("resources/prompt.json", "rb");

        if (f)
        {
            fseek(f, 0, SEEK_END);
            length = ftell(f);
            fseek(f, 0, SEEK_SET);
            payload = malloc(length);
            if (payload)
            {
                fread(payload, 1, length, f);
            }
            fclose (f);
        }

        if (payload)
        {
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

            // Set POST data
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);

            // Send all data to this function  
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);

            // Pass the 'chunk' struct to the callback function
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

            // Perform the request, res will get the return code
            res = curl_easy_perform(curl_handle);

            if(res != CURLE_OK)
            {
                snprintf(npc_response, sizeof(npc_response), "CURL Error: %s", curl_easy_strerror(res));
            }
            else
            {
                ExtractOllamaResponse(chunk.memory, npc_response, sizeof(npc_response));
            }

            // Cleanup curl handles and headers
            curl_easy_cleanup(curl_handle);
            curl_slist_free_all(headers);
        }

        free(payload);
    }

    // Free the dynamic memory chunk
    free(chunk.memory);
    curl_global_cleanup();
    
    // Unlock the generator state so the player can press 'E' again
    return NULL;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

static void
set_pixel(RenderBuffer *render, int x, int y, u32 color)
{
    int sx = (x + render->width)  % render->width;
    int sy = (y + render->height) % render->height;
    render->pixels[render->width * sy + sx] = color;
}

static void
draw_line_high(RenderBuffer *render, int x0, int y0, int x1, int y1, u32 color)
{
    int dx = x1 - x0, dy = y1 - y0, hstep = 1;
    if (dx < 0) { dx = -dx; hstep = -1; }
    int D = 2 * dx - dy, x = x0;
    for (int y = y0; y < y1; ++y) {
        set_pixel(render, x, y, color);
        if (D > 0) {
            x += hstep;
            D += 2 * (dx - dy);
        } else {
            D += 2 * dx;
        }
    }
}

static void
draw_line_low(RenderBuffer *render, int x0, int y0, int x1, int y1, u32 color)
{
    int dx = x1 - x0, dy = y1 - y0, vstep = 1;
    if (dy < 0) { dy = -dy; vstep = -1; }
    int D = 2 * dy - dx, y = y0;
    for (int x = x0; x < x1; ++x) {
        set_pixel(render, x, y, color);
        if (D > 0) {
            y += vstep;
            D += 2 * (dy - dx);
        } else {
            D += 2 * dy;
        }
    }
}

static void
draw_line(RenderBuffer *render, int x0, int y0, int x1, int y1, u32 color)
{
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1)
            draw_line_low(render, x1, y1, x0, y0, color);
        else
            draw_line_low(render, x0, y0, x1, y1, color);
    } else {
        if (y0 > y1)
            draw_line_high(render, x1, y1, x0, y0, color);
        else
            draw_line_high(render, x0, y0, x1, y1, color);
    }
}

static void
draw_ship_at(RenderBuffer *render, int x_center, int y_center, float rotation, u32 color)
{
    float c = cosf(rotation), s = sinf(rotation);
    int x0 = x_center + (int)(20*c),          y0 = y_center + (int)(20*s);
    int x1 = x_center + (int)(-10*c + 10*s),  y1 = y_center + (int)(-10*s - 10*c);
    int x2 = x_center + (int)(-10*c - 10*s),  y2 = y_center + (int)(-10*s + 10*c);
    set_pixel(render, x0, y0, color);
    draw_line(render, x0, y0, x1, y1, color);
    draw_line(render, x1, y1, x2, y2, color);
    draw_line(render, x2, y2, x0, y0, color);
}

static int
measure_text(stbtt_fontinfo font_info, const char *text, int font_size)
{
    float scale = stbtt_ScaleForPixelHeight(&font_info, font_size);
    int total = 0;
    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font_info, text[i], &advance, &lsb);
        total += (int)(advance * scale);
    }
    return total;
}

// static int
// measure_text(GameState *state, const char *text, int font_size)
// {
//     if (!state->font_buffer) return 0;
//     float scale = stbtt_ScaleForPixelHeight(&state->font, font_size);
//     int total = 0;
//     for (int i = 0; text[i]; i++) {
//         int advance, lsb;
//         stbtt_GetCodepointHMetrics(&state->font, text[i], &advance, &lsb);
//         total += (int)(advance * scale);
//     }
//     return total;
// }

float getStringWidth(stbtt_fontinfo* info, const char* text, float pixelHeight)
{
    float width = 0.0f;
    int len = strlen(text);
    
    // Calculate scale factor
    float scale = stbtt_ScaleForPixelHeight(info, pixelHeight);
    
    for (int i = 0; i < len; i++) {
        int advanceWidth, leftSideBearing;
        // Get metrics for the current character
        stbtt_GetCodepointHMetrics(info, (unsigned char)text[i], &advanceWidth, &leftSideBearing);
        width += advanceWidth;
    }
    
    return width * scale;
}   

static void
draw_message(GameState      *state,
             unsigned char  *fontbuffer,
             stbtt_fontinfo *font,
             RenderBuffer   *render,
             const char     *text,
             int             x,
             int             y,
             int             font_size)
{
    if (!state->font_buffer) return;

    float scale = stbtt_ScaleForPixelHeight(font, font_size);
    int ascent;
    stbtt_GetFontVMetrics(font, &ascent, NULL, NULL);
    int x_cursor   = x;
    int y_baseline = y + (int)(ascent * scale);
    for (int i = 0; text[i]; i++) {
        int w, h, xoff, yoff;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(font, scale, scale, text[i],
                                                         &w, &h, &xoff, &yoff);
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                unsigned char alpha = bitmap[row * w + col];
                if (!alpha) continue;
                int px = x_cursor + xoff + col;
                int py = y_baseline + yoff + row;
                if (px >= 0 && px < render->width && py >= 0 && py < render->height) {
                    u32 v = alpha;
                    render->pixels[render->width * py + px] = 0xFF000000 | (v << 16) | (v << 8) | v;
                }
            }
        }
        stbtt_FreeBitmap(bitmap, NULL);
        int advance, lsb;
        stbtt_GetCodepointHMetrics(font, text[i], &advance, &lsb);
        x_cursor += (int)(advance * scale);
    }
}

// ── Pool management ───────────────────────────────────────────────────────────

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
    pool->first_free         = pool->bullets[idx].next;
    pool->bullets[idx].next  = pool->first_used;
    pool->first_used         = idx;
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
        while (b->next != idx) {
            b = &pool->bullets[b->next];
        }
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

// ── Game logic ────────────────────────────────────────────────────────────────

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
create_asteroids_at_startup(GameState *state, RenderBuffer *render)
{
    for (int i = 0; i < 20; ++i) {
        int idx = asteroid_alloc(&state->asteroid_pool);
        Asteroid *a = &state->asteroid_pool.asteroids[idx];
        a->x_center   = (int)random_number(0, render->width);
        a->y_center   = (int)random_number(0, render->height);
        a->radius     = ASTEROID_RADIUS;
        a->is_small   = false;
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
game_init(GameState *state, RenderBuffer *render, double current_time)
{
    state->x_center                  = render->width  / 2;
    state->y_center                  = render->height / 2;
    state->last_time_hit             = current_time - INVISIBILITY_TIME;
    state->last_time_message_changed = current_time - MESSAGE_TIME;
    state->ship_color                = COLOR_WHITE;

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

    FILE *f2 = fopen("resources/ComicRelief-Regular.ttf", "rb");
    if (f2) {
        fseek(f2, 0, SEEK_END);
        long size = ftell(f2);
        fseek(f2, 0, SEEK_SET);
        state->font_buffer_comic_sans = malloc(size);
        fread(state->font_buffer_comic_sans, 1, size, f2);
        fclose(f2);
        stbtt_InitFont(&state->font_comic_sans, state->font_buffer_comic_sans, 0);
    }

    // Read the files that will be used as for the RAG
    FILE *f_kb = fopen("resources/Diario.txt", "r");
    if (f_kb) {
        fseek(f_kb, 0, SEEK_END);
        long size = ftell(f2);
        fseek(f_kb, 0, SEEK_SET);
        state->knowledge_base      = malloc(size);
        state->knowledge_base_size = (u32)size;
        fread(state->knowledge_base, 1, size, f_kb);
        fclose(f_kb);
    }

    state->message = malloc(100);

    init_asteroids(state);
    init_bullets(state);
    create_asteroids_at_startup(state, render);
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
update_ship_position(GameState *state, RenderBuffer *render, double dt)
{
    state->x_center = (state->x_center + (int)(state->x_velocity * dt) + render->width)  % render->width;
    state->y_center = (state->y_center + (int)(state->y_velocity * dt) + render->height) % render->height;
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
    if      (fabsf(diff) < step) state->rotation  = target;
    else if (diff > 0)           state->rotation += step;
    else                         state->rotation -= step;
}

static void
update_asteroids_positions(GameState *state, RenderBuffer *render, double dt)
{
    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a  = &state->asteroid_pool.asteroids[idx];
        int       dx = (int)(a->x_velocity * dt);
        int       dy = (int)(a->y_velocity * dt);
        a->x_center = (a->x_center + dx + render->width)  % render->width;
        a->y_center = (a->y_center + dy + render->height) % render->height;
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
    int     idx = bullet_alloc(&state->bullet_pool);
    Bullet *b   = &state->bullet_pool.bullets[idx];
    b->bullet_x           = state->x_center;
    b->bullet_y           = state->y_center;
    b->bullet_x_velocity  = BULLET_VELOCITY * cosf(state->rotation);
    b->bullet_y_velocity  = BULLET_VELOCITY * sinf(state->rotation);
}

static void
free_out_of_window_bullets(GameState *state, RenderBuffer *render)
{
    int idx = state->bullet_pool.first_used;
    while (idx) {
        Bullet *b    = &state->bullet_pool.bullets[idx];
        int     next = b->next;
        bool in_bound = (int)b->bullet_x > 0 && (int)b->bullet_x < render->width
                     && (int)b->bullet_y > 0 && (int)b->bullet_y < render->height;
        if (!in_bound) bullet_free(&state->bullet_pool, idx);
        idx = next;
    }
}

static void
divide_asteroid(GameState *state, int x, int y)
{
    double delta_angle = (2.0 * PI) / ASTEROID_VERTICES;
    for (int half = 0; half < 2; ++half) {
        int       idx = asteroid_alloc(&state->asteroid_pool);
        Asteroid *a   = &state->asteroid_pool.asteroids[idx];
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
        int       ax = a->x_center, ay = a->y_center;
        bool      small  = a->is_small;

        int b_idx = state->bullet_pool.first_used;
        while (b_idx) {
            Bullet *b      = &state->bullet_pool.bullets[b_idx];
            int     b_next = b->next;
            int     dx     = abs(ax - (int)b->bullet_x);
            int     dy     = abs(ay - (int)b->bullet_y);
            int     d      = (int)sqrtf((float)(Square(dx) + Square(dy)));
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
        Asteroid *a  = &state->asteroid_pool.asteroids[idx];
        int       dx = abs(a->x_center - state->x_center);
        int       dy = abs(a->y_center - state->y_center);
        int       d  = (int)sqrtf((float)(Square(dx) + Square(dy)));
        if (d < ASTEROID_RADIUS + 15) {
            state->hitted = true;
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

static void
draw_asteroids(GameState *state, RenderBuffer *render)
{
    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a  = &state->asteroid_pool.asteroids[idx];
        int       x0 = a->x_vertices[0], y0 = a->y_vertices[0];
        for (int j = 1; j < ASTEROID_VERTICES + 1; ++j) {
            int x1 = a->x_vertices[j % ASTEROID_VERTICES];
            int y1 = a->y_vertices[j % ASTEROID_VERTICES];
            draw_line(render, x0, y0, x1, y1, COLOR_WHITE);
            x0 = x1; y0 = y1;
        }
        idx = a->next;
    }
}

static void
draw_bullets(GameState *state, RenderBuffer *render)
{
    int idx = state->bullet_pool.first_used;
    while (idx) {
        Bullet *b = &state->bullet_pool.bullets[idx];
        set_pixel(render, b->bullet_x, b->bullet_y, COLOR_WHITE);
        idx = b->next;
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

void
game_update_and_render(GameMemory *memory, GameInput *input, GameAudio *audio, RenderBuffer *render)
{
    GameState *state = (GameState *)memory->permanent_storage;

    // Detect layout changes from hot reload and reinitialize cleanly
    if (state->state_size != sizeof(GameState)) {
        memset(state, 0, sizeof(GameState));
        state->state_size = sizeof(GameState);
    }

    if (!state->x_center && !state->y_center) game_init(state, render, input->current_time);

    audio->play_hit_sound = false;

    double dt = input->dt;

    update_ship_velocity(state, input);
    update_ship_position(state, render, dt);
    update_ship_rotation(state, dt);

    if (input->bullet_shot) shoot_bullet_from_ship(state);

    update_asteroids_positions(state, render, dt);
    update_bullets_positions(state, dt);
    free_out_of_window_bullets(state, render);

    check_asteroid_ship_collision(state, input->current_time);
    if (state->hitted) {
        audio->play_hit_sound = true;
        state->last_time_hit  = input->current_time;
        state->hitted         = false;
        state->x_center       = render->width  / 2;
        state->y_center       = render->height / 2;
    }

    check_invisibility(state, input->current_time);
    check_asteroid_bullet_collision(state);

    memset(render->pixels, 0, 4 * render->width * render->height);

    draw_ship_at(render, state->x_center, state->y_center, state->rotation, state->ship_color);
    draw_asteroids(state, render);
    draw_bullets(state, render);

    char str_score[20];
    snprintf(str_score, sizeof(str_score), "%d", state->score);
    int w = measure_text(state->font, str_score, 30);
    draw_message(state, state->font_buffer, &state->font,
        render, str_score, render->width - w - 10, 10, 30);

    // TODO
    // Draw the message near the ship
    // Can I draw a simple square and fit the text inside it?
    // That would mean to have some wrapping behavior for the text

    // Now instead of this message I want a message that have to be written using RAG techiniques
    // So let's prepare a text that will be read by the program at startup
    // Now let's go with the RAG (that is not RAG)
    // Let's have an LLM and pass the Diario entirely in the prompt
    if (input->current_time - state->last_time_message_changed > MESSAGE_TIME) {
        // Spawn thread for receiving response asynchronously from llm server
        const char* player_prompt = "How are you?";

        // Considering that in FetchLLMDialogue I use WriteMemoryCallback
        // Do I really need to spawn a thread?
        // I was thinking of inlining FetchLLMDialogue here
        // What would be the flow in that case?

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, FetchLLMDialogue, (void *)player_prompt);
        pthread_detach(thread_id);

        state->last_time_message_changed = input->current_time;
    }

    // Look at npc_response and get its size in pixels
    int llm_text_size = 20;

    float llm_text_len_total = getStringWidth(&state->font_comic_sans, npc_response, (float)llm_text_size);

    // I need to know the height in pixels of my font
    // float scale = stbtt_ScaleForPixelHeight(&state->font_comic_sans, llm_text_size);
    // int ascent;
    // stbtt_GetFontVMetrics(&state->font_comic_sans, &ascent, NULL, NULL);

    int llm_text_width  = llm_text_len_total;

    char *ch = npc_response;
    int npc_response_len = 0;
    while (*ch != '\0')
    {
        ++npc_response_len;
        ++ch;
    }

    // Let's divide npc response in 4 parts
    char *npc_response_1 = malloc(100);
    char *npc_response_2 = malloc(100);
    char *npc_response_3 = malloc(100);
    char *npc_response_4 = malloc(100);

    strncpy(npc_response_1, npc_response,                            npc_response_len / 4);
    strncpy(npc_response_2, npc_response +     npc_response_len / 4, npc_response_len / 4);
    strncpy(npc_response_3, npc_response +     npc_response_len / 2, npc_response_len / 4);
    strncpy(npc_response_4, npc_response + 3 * npc_response_len / 4, npc_response_len / 4);


    // I want to draw a bounding box with proportion 4:1 around the text generated by the llm
    int box_padding = 5;

    // Horizontal lines
    draw_line(render, state->x_center, state->y_center - box_padding,
              state->x_center + llm_text_width / 4, state->y_center - box_padding, COLOR_WHITE);

    draw_line(render, state->x_center, state->y_center + 4 * llm_text_size + box_padding,
              state->x_center + llm_text_width / 4, state->y_center + 4 * llm_text_size + box_padding, COLOR_WHITE);

    // Vertical lines
    draw_line(render, state->x_center, state->y_center - box_padding,
              state->x_center, state->y_center + 4 * llm_text_size + box_padding, COLOR_WHITE);

    draw_line(render, state->x_center + llm_text_width / 4, state->y_center - box_padding,
              state->x_center + llm_text_width / 4, state->y_center + 4 * llm_text_size + box_padding, COLOR_WHITE);

    draw_message(state, state->font_buffer_comic_sans, &state->font_comic_sans,
                 render, npc_response_1, state->x_center, state->y_center, llm_text_size);

    draw_message(state, state->font_buffer_comic_sans, &state->font_comic_sans,
                 render, npc_response_2, state->x_center, state->y_center + llm_text_size, llm_text_size);

    draw_message(state, state->font_buffer_comic_sans, &state->font_comic_sans,
                 render, npc_response_3, state->x_center, state->y_center + 2 * llm_text_size, llm_text_size);

    draw_message(state, state->font_buffer_comic_sans, &state->font_comic_sans,
                 render, npc_response_4, state->x_center, state->y_center + 3 * llm_text_size, llm_text_size);
    
    free(npc_response_1);
    free(npc_response_2);
    free(npc_response_3);
    free(npc_response_4);
}
