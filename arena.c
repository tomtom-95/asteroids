#include <stdlib.h>
#include <stdbool.h>

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

u8 *
arena_push(Arena *arena, u64 size, u64 alignment)
{
    Arena *curr = arena->curr;

    curr->pos = AlignPow2(curr->pos, alignment);

    if (curr->pos + size > curr->capacity) {
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
    curr->pos   = global_pos - curr->base_pos;
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

void
scratch_alloc(void)
{
    scratch_arenas[0] = arena_alloc(KB(64));
    scratch_arenas[1] = arena_alloc(KB(64));
}

Arena *
tctx_get_scratch(Arena **conflicts, u64 count)
{
    for (u32 i = 0; i < ArraySize(scratch_arenas); ++i) {
        bool conflict = false;
        for (u32 j = 0; j < count; ++j) {
            if (scratch_arenas[i] == conflicts[j]) {
                conflict = true;
            }
        }
        if (!conflict) {
            return scratch_arenas[i];
        }
    }

    return NULL;
}
