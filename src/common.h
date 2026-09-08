#ifndef _BS_COMMON_H_
#define _BS_COMMON_H_

#include <stdbool.h>
#if !defined(__cplusplus) || (__cplusplus < 201103L && !defined(nullptr))
#define nullptr NULL
#endif

#include <stdint.h>

/* on some platforms, stdint.h exists but is incomplete */
#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFU
#endif
#ifndef INT32_MAX
#define INT32_MAX 0x7FFFFFFF
#endif
#ifndef INT32_MIN
#define INT32_MIN (-INT32_MAX - 1)
#endif

#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || defined(__BIG_ENDIAN__)
#define IS_BIG_ENDIAN
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
    #define MAYBE_UNUSED [[maybe_unused]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    #define MAYBE_UNUSED [[maybe_unused]]
#elif defined(__GNUC__) || defined(__clang__)
    #define MAYBE_UNUSED __attribute__((unused))
#else
    #define MAYBE_UNUSED
#endif

#if (defined(__GNUC__) && (__GNUC__ >= 3 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 8))) || defined(__clang__) || defined(__TINYC__)
    #define BS_ALIGN(x) __attribute__((aligned(x)))
#else
    #define BS_ALIGN(x)
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(__TINYC__)
    #define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 // VS2005 or later
    #define NOINLINE __declspec(noinline)
#else
    #define NOINLINE
#endif

#if defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
        #define YIELD() __asm__ volatile("rep; nop" : : : "memory")
    #elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && (__ARM_ARCH >= 7))
        #define YIELD() __asm__ volatile("yield" : : : "memory")
    #elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
        #define YIELD() __asm__ volatile("or 27, 27, 27" : : : "memory")
    #elif defined(__mips__)
        #define YIELD() __asm__ volatile(".word 0x00000140" : : : "memory")
    #elif defined(__loongarch__)
        #define YIELD() __asm__ volatile("ibar 0" : : : "memory")
    #elif defined(__riscv)
        #define YIELD() __asm__ volatile(".insn i 0x0F, 0, x0, x0, 1" : : : "memory")
    #else
        #define YIELD() ((void)0)
    #endif
#elif defined(_MSC_VER)
    #if (defined(_M_X64) || defined(_M_IX86)) && _MSC_VER >= 1400
        #include <intrin.h>
        #define YIELD() _mm_pause()
    #elif defined(_M_ARM64) || defined(_M_ARM)
        #include <intrin.h>
        #define YIELD() __yield()
    #else
        #define YIELD() ((void)0)
    #endif
#else
    #define YIELD() ((void)0)
#endif

#ifdef _MSC_VER
#define longlong __int64
#else
#define longlong long long
#endif

#endif /* _BS_COMMON_H_ */
