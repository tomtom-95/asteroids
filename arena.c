#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include "arena.h"

Arena *
arena_alloc(u64 capacity)
{
    Arena *arena = malloc(ARENA_HEADER_SIZE + capacity);

    arena->prev = NULL;
    arena->curr = arena;

    arena->base_pos = 0;
    arena->pos      = ARENA_HEADER_SIZE;
    arena->capacity = capacity;

    return arena;
}

void
arena_free(Arena *arena)
{
    Arena *curr = arena->curr;

    while (curr) {
        Arena *prev = curr->prev;
        free(curr);
        curr = prev;
    }
}

void *
arena_start(Arena *arena)
{
    return (u8 *)arena + ARENA_HEADER_SIZE;
}

void *
arena_push(Arena *arena, u64 size, u64 alignment)
{
    assert(size < arena->capacity);

    Arena *curr = arena->curr;

    curr->pos = AlignPow2(curr->pos, alignment);

    if (curr->pos + size > ARENA_HEADER_SIZE + curr->capacity) {
        // Chain a new arena block
        Arena *next = malloc(ARENA_HEADER_SIZE + arena->capacity);

        next->prev     = curr;
        next->curr     = next;

        next->base_pos = curr->base_pos + ARENA_HEADER_SIZE + curr->capacity;
        next->pos      = ARENA_HEADER_SIZE;
        next->capacity = arena->capacity;

        arena->curr = next;

        curr = next;
    }

    // I can allocate now and I am sure that curr is an arena block with enough space
    curr->pos = AlignPow2(curr->pos, alignment);
    u64 start = curr->pos;
    curr->pos += size;

    // Return to the user a pointer to the start of the space he can write to
    return (u8 *)(curr) + start;
}

void
arena_pop_to(Arena *arena, u64 global_pos)
{
    Arena *curr = arena->curr;
    while (curr->base_pos > global_pos) {
        Arena *prev = curr;
        curr = curr->prev;
        free(prev);
    }

    arena->curr = curr;
    if (global_pos == curr->base_pos) {
        curr->pos = ARENA_HEADER_SIZE;
    } else {
        curr->pos = global_pos - curr->base_pos;
    }
}

u64
arena_global_position(Arena *arena)
{
    Arena *curr = arena->curr;
    u64 pos = curr->base_pos + curr->pos;
    return pos;
}

Temp
temp_begin(Arena *arena)
{
    Temp temp = {arena, arena_global_position(arena)};
    return temp;
}

void
temp_end(Temp temp)
{
    arena_pop_to(temp.arena, temp.pos);
}

TCTX *
tctx_alloc(void)
{
    Arena *arena_0 = arena_alloc(KB(64));
    Arena *arena_1 = arena_alloc(KB(64));

    TCTX *tctx = array_push(arena_0, TCTX, 1);
    tctx->scratch_arenas[0] = arena_0;
    tctx->scratch_arenas[1] = arena_1;

    return tctx;
}

void
tctx_release(TCTX *tctx)
{
    arena_free(tctx->scratch_arenas[0]);
    arena_free(tctx->scratch_arenas[1]);
}


void
tctx_select(TCTX *tctx)
{
    tctx_thread_local = tctx;
}

TCTX *
tctx_selected(void)
{
    return tctx_thread_local;
}

Arena *
tctx_get_scratch(Arena **conflicts, u64 count)
{
    TCTX *tctx = tctx_selected();

    for (u32 i = 0; i < ArraySize(tctx->scratch_arenas); ++i)
    {
        bool conflict = false;
        for (u32 j = 0; j < count; ++j)
        {
            if (tctx->scratch_arenas[i] == conflicts[j])
            {
                conflict = true;
            }
        }
        if (!conflict)
        {
            return tctx->scratch_arenas[i];
        }
    }

    return NULL;
}
