#ifndef ARENA_H
#define ARENA_H

#include <string.h>

#include "base.h"

#define ARENA_HEADER_SIZE 128

typedef struct Arena Arena;
struct Arena {
    Arena *prev;
    Arena *curr;

    // Global position, start of the arena block in the linked list in the global coordinate space
    u64 base_pos;

    // Local position
    u64 pos;

    u64 capacity;
};
_Static_assert(sizeof(Arena) <= ARENA_HEADER_SIZE, "ARENA_HEADER_SIZE is too small");

typedef struct Temp Temp;
struct Temp
{
  Arena *arena;
  u64    pos;
};

// Globals scratch arenas
Arena *scratch_arenas[2];

Arena *arena_alloc(u64 capacity);
void   arena_free(Arena *arena);

u64 arena_global_position(Arena *arena);

u8   *arena_push(Arena *arena, u64 size, u64 alignment);
void  arena_pop_to(Arena *arena, u64 global_pos);

Temp temp_begin(Arena *arena);
void temp_end(Temp temp);

#define array_push(arena, type, count) arena_push(arena, sizeof(type) * count, AlignOf(type))

// Scratch 
void   scratch_alloc(void);
Arena *tctx_get_scratch(Arena **conflicts, u64 count);

#define scratch_begin(conflicts, count) temp_begin(tctx_get_scratch(conflicts, count))
#define scratch_end(temp) temp_end(temp)

#endif // ARENA_H