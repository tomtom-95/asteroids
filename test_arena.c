#include "base.h"
#include "arena.c"

#include <stdlib.h>
#include <stdbool.h>

// TODO: let's pretend for now that the implementation of the arena allocator is bug free
//       when I feel like I will add tests and I fill use the free list approach for freeing
//       arena block (right now I call free() instead of putting blocks into a free list)

void
bar(Arena *arena)
{
    Temp temp = scratch_begin(&arena, 1);

    u8 *myarray = array_push(arena, int, 10);
    for (int i = 0; i < 10; ++i) {
        myarray[i] = i;
    }

    scratch_end(temp);
}

void
foo(Arena *arena)
{
    Temp temp = scratch_begin(&arena, 1);

    bar(temp.arena);

    scratch_end(temp);
}

void
test_chain_block(void)
{
    // Test that the creation of a new block in the chain actually works
    Arena *arena = arena_alloc(32);

    array_push(arena, u8, 30);
    array_push(arena, u8, 8);

    assert(arena->curr->base_pos == arena->capacity + ARENA_HEADER_SIZE);
}

void
test_pop_block(void)
{
    Arena *arena = arena_alloc(32);

    array_push(arena, u8, 16);

    Temp temp = temp_begin(arena);

    array_push(temp.arena, u8, 14);
    array_push(temp.arena, u8, 8);

    temp_end(temp);

    assert(arena->curr->pos == ARENA_HEADER_SIZE + 16);
}

int
main(void)
{
    // test_chain_block();
    test_pop_block();

    return 0;
}
