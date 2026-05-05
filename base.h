#pragma once
#include <stdint.h>

#define Max(a, b)      ((a) > (b) ? (a) : (b))
#define Min(a, b)      ((a) < (b) ? (a) : (b))
#define Square(x)      ((x) * (x))
#define ArraySize(a)   (sizeof(a) / sizeof((a)[0]))

#ifndef PI
#define PI 3.14159265358979323846
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
