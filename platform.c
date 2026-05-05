#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <raylib.h>

#include "base.h"
#include "game.h"

#define GAME_DLL_PATH     "build/game.dylib"
#define GAME_DLL_TMP_PATH "build/game_tmp.dylib"

typedef struct {
    void                    *handle;
    game_update_and_render_t update_and_render;
    time_t                   mtime;
} GameDLL;

static time_t
file_mtime(const char *path)
{
    struct stat s;
    return (stat(path, &s) == 0) ? s.st_mtime : 0;
}

static GameDLL
load_game_dll(void)
{
    GameDLL dll = {0};

    // Copy so we can overwrite game.dylib during a rebuild without issues
    system("cp " GAME_DLL_PATH " " GAME_DLL_TMP_PATH);

    dll.handle = dlopen(GAME_DLL_TMP_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!dll.handle) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return dll;
    }

    dll.update_and_render = (game_update_and_render_t)dlsym(dll.handle, "game_update_and_render");
    if (!dll.update_and_render) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        dlclose(dll.handle);
        dll.handle = NULL;
        return dll;
    }

    dll.mtime = file_mtime(GAME_DLL_PATH);
    return dll;
}

static void
unload_game_dll(GameDLL *dll)
{
    if (dll->handle) dlclose(dll->handle);
    dll->handle = NULL;
    dll->update_and_render = NULL;
}

int
main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIDTH, HEIGHT, "Asteroids");
    InitAudioDevice();

    Sound hit_sound = LoadSound("resources/fart2.wav");

    u32 *pixels = malloc(WIDTH * HEIGHT * sizeof(u32));

    GameState *state = calloc(1, GAME_STATE_MAX_SIZE);
    state->pixels = pixels;
    state->width  = WIDTH;
    state->height = HEIGHT;

    Image img = {
        .data    = pixels,
        .width   = WIDTH,
        .height  = HEIGHT,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    Texture2D tex = LoadTextureFromImage(img);

    GameDLL game = load_game_dll();

    SetTargetFPS(60);

    double last_time = GetTime();

    while (!WindowShouldClose()) {
        // Hot reload when game.dylib changes on disk
        time_t mtime = file_mtime(GAME_DLL_PATH);
        if (mtime && mtime != game.mtime) {
            unload_game_dll(&game);
            game = load_game_dll();
        }

        double now = GetTime();

        GameInput input = {
            .left        = IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A),
            .right       = IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D),
            .up          = IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W),
            .down        = IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S),
            .bullet_shot = IsKeyDown(KEY_SPACE),
            .current_time = now,
            .dt           = now - last_time,
        };
        last_time = now;

        GameAudio audio = {0};

        if (game.update_and_render)
            game.update_and_render(state, &input, &audio);

        if (audio.play_hit_sound) PlaySound(hit_sound);

        UpdateTexture(tex, pixels);

        BeginDrawing();
            ClearBackground(BLACK);
            Rectangle src = { 0, 0, WIDTH, HEIGHT };
            Rectangle dst = { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() };
            DrawTexturePro(tex, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        EndDrawing();
    }

    unload_game_dll(&game);
    UnloadTexture(tex);
    UnloadSound(hit_sound);
    CloseAudioDevice();
    CloseWindow();
    free(pixels);
    free(state);

    return 0;
}
