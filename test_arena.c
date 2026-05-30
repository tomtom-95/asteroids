#include "base.h"
#include "arena.c"

#include <stdlib.h>
#include <stdbool.h>


void
bar(Arena *arena)
{
    Temp temp = scratch_begin(&arena, 1);

    // I want this to survive the return to the caller
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

int
main(void)
{
    scratch_alloc();

    Arena *arena = arena_alloc(MB(1));
    arena_free(arena);

    arena = arena_alloc(MB(100));

    array_push(arena, int, 10);
    array_push(arena, s32, 100);

    foo(arena);

    return 0;
}
