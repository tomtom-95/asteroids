#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <curl/curl.h>
#include <stdatomic.h>

#include "base.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "game.h"
#include "arena.c"

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

#define TEXTBOX_HEIGHT 100


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
    // first field — detects layout changes on hot reload
    size_t state_size;

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

    // Data for sending request to and receive response from llm
    CURL *curl;

    // Assume for now that the response I receive from curl will never exceed 300 char
    char  read_buffer[600];
    u64   read_buffer_len;

    char  llm_response[300];
    char *player_prompt;

    atomic_bool thread_running;
    pthread_mutex_t llm_response_mutex;
} GameState;

// Callback function for libcurl to write incoming data chunks
size_t
WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    GameState *game_state = (GameState *)userp;

    u64 space = sizeof(game_state->read_buffer) - 1 - game_state->read_buffer_len;

    // How many bytes can I actually copy without overflowing
    size_t n = real_size < space ? real_size : space;

    memcpy(game_state->read_buffer + game_state->read_buffer_len, contents, n);
    game_state->read_buffer_len += n;
    game_state->read_buffer[game_state->read_buffer_len] = 0;

    // return the full amount so curl doesn't error
    return real_size;   
}

// Simple quick-and-dirty helper to extract the "response" field from Ollama's JSON
// For complex games, consider using a proper library like cJSON.

// While the response is being writtin into llm_response I want no other thread (in this case the main one)
// To access the resource, I think this is the place, but let's think about what would happen
// If I put a mutex here will it block the main thread until it finishes the copy?
void
ExtractOllamaResponse(const char* json_str, char* llm_response, size_t dest_max)
{
    const char* key = "\"response\":\"";
    const char* start = strstr(json_str, key);

    if (start)
    {
        start += strlen(key);
        size_t i = 0;
        
        // Copy until the closing unescaped quote (ignoring simple \" for now)
        while (*start && *start != '"' && i < dest_max - 1)
        {
            // Handle simple escape characters if necessary
            if (*start == '\\' && *(start + 1) == 'n')
            {
                llm_response[i++] = '\n';
                start += 2;
            }
            else
            {
                llm_response[i++] = *start;
                start++;
            }
        }
        llm_response[i] = '\0';
    }
    else
    {
        snprintf(llm_response, dest_max, "Error: Could not parse response.");
    }
}

// The background worker function
// A thread is created with pthread_create in game.c
// So this function runs on the thread and performs a curl
// In game_init you can see how the curl is set up, in particular the function that will be called
// When something returns from the API call is WriteCallback
// This function gets called when the curl request has something to return
// The function will copy contents into the game_state->read_buffer
// So this means that I want to keep reading stuff into the game_state->read_buffer and clear it only
// When a new request is done so that a new message arrive

// So this is what is happening on the thread, in the meantime what is happening on the main loop?
// In the main loop we print state->llm_response on the screen (line 840 of game.c)
// So when state->llm_response is actually written?
// It is ExtractOllamaResponse that is responsible for taking what is stored in the read_buffer and
// write the string into game_state->llm_response (after parsing of the json)
// So this means that also ExtractOllamaResponse is running in the thread?
// So I have game_state->llm_response that is updated in the thread and then is read in the game
// main loop. Can this be a problem in terms of race condition?
// Really study and analyze what is going on, it is the first time I am studying something thread
// related and async call. I need an as much as possible in depth explanation on what is happening
// and what problems might arise.

// Current problems in the code caused by the fact that we are using a thread
void *
FetchLLMDialogue(void* _game_state)
{
    GameState *game_state = (GameState *)_game_state;

    CURL *curl = game_state->curl;

    // The json that the post request return
    char *read_buffer = game_state->read_buffer;

    if (curl)
    {
        // Reset to 0 the buffer len so that I can write the new response from the llm
        game_state->read_buffer_len = 0;

        CURLcode res = curl_easy_perform(curl);

        pthread_mutex_lock(&game_state->llm_response_mutex);
        if(res != CURLE_OK)
        {
            snprintf(game_state->llm_response, sizeof(game_state->llm_response),
                     "CURL Error: %s", curl_easy_strerror(res));
        }
        else
        {
            ExtractOllamaResponse(read_buffer, game_state->llm_response, sizeof(game_state->llm_response));
        }
        pthread_mutex_unlock(&game_state->llm_response_mutex);
    }

    // Release the flag last, after llm_response is fully written, so the main
    // thread can spawn the next request. This store also becomes the
    // synchronization point we will lean on when we fix the llm_response race.
    atomic_store(&game_state->thread_running, false);
    return NULL;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

static void
set_pixel(RenderBuffer *render, int x, int y, u32 color)
{
    // Wrap (toroidal) into the view's own bounds.
    int sx = x % render->width;
    if (sx < 0) sx += render->width;
    int sy = y % render->height;
    if (sy < 0) sy += render->height;

    render->pixels[render->stride * sy + sx] = color;
}

// A view into a sub-rectangle of `parent`. Shares the parent's memory and
// stride; drawing wraps within [0,width) x [0,height) of the sub-region.
static RenderBuffer
sub_buffer(RenderBuffer *parent, int x, int y, int width, int height)
{
    return (RenderBuffer){
        .pixels = parent->pixels + (y * parent->stride + x),
        .width  = width,
        .height = height,
        .stride = parent->stride,
    };
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
draw_line_low(RenderBuffer *render, int x0, int y0, int x1, int y1,
              u32 color)
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
draw_line(RenderBuffer *render,
          int x0, int y0, int x1, int y1,
          u32 color)
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

static void
draw_rectangle(RenderBuffer *render, int x, int y, int width, int height, u32 color)
{
    // Horizontal lines
    draw_line(render, x, y, x + width, y, color);
    draw_line(render, x, y + height, x + width, y + height, color);

    // Vertical lines
    draw_line(render, x, y, x, y + height, color);
    draw_line(render, x + width, y, x + width, y + height, color);
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
        unsigned char *bitmap = stbtt_GetCodepointBitmap(font, scale, scale, text[i], &w, &h, &xoff, &yoff);
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                unsigned char alpha = bitmap[row * w + col];
                if (!alpha) continue;
                int px = x_cursor + xoff + col;
                int py = y_baseline + yoff + row;
                if (px >= 0 && px < render->width && py >= 0 && py < render->height) {
                    u32 v = alpha;
                    render->pixels[render->stride * py + px] = 0xFF000000 | (v << 16) | (v << 8) | v;
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
game_init(Arena *arena, GameState *state, RenderBuffer *render, double current_time)
{
    state->read_buffer_len = 0;

    state->curl = curl_easy_init();
    if(state->curl)
    {
        curl_easy_setopt(state->curl, CURLOPT_URL, "http://localhost:11434/api/generate");
        curl_easy_setopt(state->curl, CURLOPT_POST, 1L);

        // stream:false -> Ollama returns ONE JSON object with a single
        // "response" field, which is what ExtractOllamaResponse expects.
        curl_easy_setopt(state->curl, CURLOPT_POSTFIELDS, "{\"model\":\"gemma3:4b\",\"prompt\":\"Say whatever you want in 10 words.\",\"stream\":false}");

        struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(state->curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(state->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(state->curl, CURLOPT_WRITEDATA, state);
    }

    state->x_center                  = render->width  / 2;
    state->y_center                  = render->height / 2;
    state->last_time_hit             = current_time - INVISIBILITY_TIME;
    state->last_time_message_changed = current_time - MESSAGE_TIME;
    state->ship_color                = COLOR_RED;
    state->state_size                = sizeof(GameState);

    state->thread_running = false;
    pthread_mutex_init(&state->llm_response_mutex, NULL);

    FILE *f = fopen("resources/PressStart2P-Regular.ttf", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        state->font_buffer = array_push(arena, u8, size);
        fread(state->font_buffer, 1, size, f);
        fclose(f);
        stbtt_InitFont(&state->font, state->font_buffer, 0);
    }

    FILE *f2 = fopen("resources/ComicRelief-Regular.ttf", "rb");
    if (f2) {
        fseek(f2, 0, SEEK_END);
        long size = ftell(f2);
        fseek(f2, 0, SEEK_SET);
        state->font_buffer_comic_sans = array_push(arena, u8, size);
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
        state->knowledge_base      = array_push(arena, u8, size);
        state->knowledge_base_size = (u32)size;
        fread(state->knowledge_base, 1, size, f_kb);
        fclose(f_kb);
    }

    state->message = array_push(arena, u8, 100);

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
        state->ship_color = COLOR_GREEN;
}

static void
draw_asteroids(GameState *state, RenderBuffer *render)
{
    int idx = state->asteroid_pool.first_used;
    while (idx) {
        Asteroid *a  = &state->asteroid_pool.asteroids[idx];
        int x0 = a->x_vertices[0];
        int y0 = a->y_vertices[0];
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
game_update_and_render(GameMemory *game_memory, GameInput *input, GameAudio *audio, RenderBuffer *render)
{
    RenderBuffer game_window = sub_buffer(render, 0, 0, render->width, render->height - TEXTBOX_HEIGHT);
    RenderBuffer textbox_render = sub_buffer(render, 0, render->height - TEXTBOX_HEIGHT, render->width, TEXTBOX_HEIGHT);

    arena_pop_to(game_memory->transient_memory, 0);

    GameState *state = arena_start(game_memory->persistent_memory);

    // Make sure that the GameState struct has not changed after hot reloading
    if (game_memory->is_initialized) {
        assert(state->state_size == sizeof(GameState));
    } else {
        state = array_push(game_memory->persistent_memory, GameState, 1);
        game_memory->is_initialized = true;
    }

    if (!state->x_center && !state->y_center) game_init(game_memory->persistent_memory, state, &game_window, input->current_time);

    audio->play_hit_sound = false;

    double dt = input->dt;

    update_ship_velocity(state, input);
    update_ship_position(state, &game_window, dt);
    update_ship_rotation(state, dt);

    if (input->bullet_shot) shoot_bullet_from_ship(state);

    update_asteroids_positions(state, &game_window, dt);
    update_bullets_positions(state, dt);
    free_out_of_window_bullets(state, &game_window);

    check_asteroid_ship_collision(state, input->current_time);
    if (state->hitted) {
        audio->play_hit_sound = true;
        state->last_time_hit  = input->current_time;
        state->hitted         = false;
        state->x_center       = game_window.width  / 2;
        state->y_center       = game_window.height / 2;
    }

    check_invisibility(state, input->current_time);
    check_asteroid_bullet_collision(state);

    // Clear the render buffer before drawing
    memset(render->pixels, 0, 4 * render->width * render->height);

    draw_ship_at(&game_window, state->x_center, state->y_center, state->rotation, state->ship_color);
    draw_asteroids(state, &game_window);
    draw_bullets(state, &game_window);

    char str_score[20];
    snprintf(str_score, sizeof(str_score), "%d", state->score);
    int w = measure_text(state->font, str_score, 30);
    draw_message(state, state->font_buffer, &state->font, &game_window, str_score, game_window.width - w - 10, 10, 30);

    // Move the creation of the thread in the platform layer
    // Why? I think that the idea is that thread creation is a service that the platform layer offers to the
    // game, similar to setting up memory, input, audio and renderer

    // So in the platform layer I must define how thread are spawned? That does not seem ideal
    // Let's first think about the stuff I was worried about, which is handling correctly the
    // thread concurrency problem, then I will handle the platform problems

    // The function that the thread executes is FetchLLMDialogue
    // this function have knowledge of GameState
    // this is wrong, what if MESSAGE_TIME pass and FetchLLMDialogue is still running in the previous thread?
    // I do not want to spawn another thread while the other is still running
    // So I need a variable that can be set atomically to one variable from the thread 

    // The second fix I have to make is related to the fact that while the child thread is writing stuff into
    // the string the main thread can start reading from it

    // Only fire a new request once MESSAGE_TIME has elapsed since the last one,
    // AND only if no worker is currently running. The short-circuit matters:
    // when the timer hasn't elapsed we never touch thread_running. When it has,
    // atomic_exchange flips the flag to true and tells us whether a worker was
    // already in flight (if so we skip and leave the flag set).
    bool time_for_new_message = (input->current_time - state->last_time_message_changed) >= MESSAGE_TIME;

    if (time_for_new_message && !atomic_exchange(&state->thread_running, true))
    {
        // Spawn thread for receiving response asynchronously from llm server
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, FetchLLMDialogue, (void *)state);
        pthread_detach(thread_id);

        state->last_time_message_changed = input->current_time;
    }

    draw_rectangle(&textbox_render, 0, 0, textbox_render.width, textbox_render.height, COLOR_WHITE);

    char response_copy[300];
    pthread_mutex_lock(&state->llm_response_mutex);
    memcpy(response_copy, state->llm_response, sizeof(response_copy));
    pthread_mutex_unlock(&state->llm_response_mutex);

    unsigned char *font = state->font_buffer_comic_sans;
    stbtt_fontinfo *font_info = &state->font_comic_sans;
    draw_message(state, font, font_info, &textbox_render, response_copy, 0, 0, 20);
}
