#ifndef BASE_H
#define BASE_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

////////////////////////////////
//~ rjf: Units

#define KB(n)  (((u64)(n)) << 10)
#define MB(n)  (((u64)(n)) << 20)
#define GB(n)  (((u64)(n)) << 30)
#define TB(n)  (((u64)(n)) << 40)

#define Thousand(n)   ((n)*1000)
#define Million(n)    ((n)*1000000)
#define Billion(n)    ((n)*1000000000)

#define Max(a, b)      ((a) > (b) ? (a) : (b))
#define Min(a, b)      ((a) < (b) ? (a) : (b))
#define Square(x)      ((x) * (x))
#define ArraySize(a)   (sizeof(a) / sizeof((a)[0]))

#ifndef PI
#define PI 3.14159265358979323846
#endif

////////////////////////////////
//~ rjf: Clang OS/Arch Cracking

#if defined(__clang__)
    #define COMPILER_CLANG 1

    #if defined(_WIN32)
        #define OS_WINDOWS 1
    #elif defined(__gnu_linux__) || defined(__linux__)
        #define OS_LINUX 1
    #elif defined(__APPLE__) && defined(__MACH__)
        #define OS_MAC 1
    #else
        #error This compiler/OS combo is not supported.
    #endif

    #if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
        #define ARCH_X64 1
    #elif defined(i386) || defined(__i386) || defined(__i386__)
        #define ARCH_X86 1
    #elif defined(__aarch64__)
        #define ARCH_ARM64 1
    #elif defined(__arm__)
        #define ARCH_ARM32 1
    #else
        #error Architecture not supported.
    #endif
#endif

////////////////////////////////
//~ rjf: Type -> Alignment

#if COMPILER_MSVC
    #define AlignOf(T) __alignof(T)
#elif COMPILER_CLANG
    #define AlignOf(T) __alignof(T)
#elif COMPILER_GCC
    #define AlignOf(T) __alignof__(T)
#else
    #error AlignOf not defined for this compiler.
#endif

#define AlignPow2(x,b)     (((x) + (b) - 1)&(~((b) - 1)))
#define AlignDownPow2(x,b) ((x)&(~((b) - 1)))
#define AlignPadPow2(x,b)  ((0-(x)) & ((b) - 1))
#define IsPow2(x)          ((x)!=0 && ((x)&((x)-1))==0)
#define IsPow2OrZero(x)    ((((x) - 1)&(x)) == 0)

#endif // BASE_H