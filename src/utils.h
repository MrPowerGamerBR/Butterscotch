#ifndef _BS_UTILS_H_
#define _BS_UTILS_H_

#include "common.h"
#include <stdarg.h>
#include "stdio_compat.h"
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include "string_compat.h"
#include "math_compat.h"

#include "real_type.h"

#include "log.h"

#ifdef PLATFORM_PS2
#include <malloc.h>
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
    #define TYPEOF(x) typeof(x)
#elif defined(_MSC_VER) && defined(__cplusplus) && __cplusplus >= 201103L
    #define TYPEOF(x) std::remove_reference<decltype(x)>::type
#elif defined(__GNUC__) || defined(__clang__) || \
    (defined(__TINYC__) && __TINYC__ >= 913) || \
    (defined(_MSC_VER) && _MSC_VER >= 1940 && !defined(__cplusplus))
    #define TYPEOF(x) __typeof__(x)
#else
    #define TYPEOF(x) int64_t
#endif

#define forEach(type, item, array, count) \
    for (TYPEOF(count) item##_i_ = 0; item##_i_ < (count); ++item##_i_) \
    for (type* item = &(array)[item##_i_]; item; item = NULL)

#define forEachIndexed(type, item, index, array, count) \
    for (TYPEOF(count) index = 0; index < (count); ++index) \
    for (type* item = &(array)[index]; item; item = NULL)

#define repeat(n, it) for (TYPEOF(n) it = 0; it < (n); ++it)

#define require(condition) \
    do { \
        if (!(condition)) { \
        logError("Requirement failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define requireMessage(condition, message) \
    do { \
        if (!(condition)) { \
        logError("Requirement failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
        abort(); \
	} \
} while (0)

static inline void requireMessageFormatted(const char *file, int line, bool condition, const char *fmt, ...) {
    if (condition)
        return;
    va_list args;
    logError("Requirement failed at %s:%d: ", file, line);
    va_start(args, fmt);
    vLogError(fmt, args);
    va_end(args);
    logError("\n");
    abort();
}

static inline void* requireNotNullFunction(void* ptr, const char* file, int line, const char* name) {
    if (!ptr) {
        logError("%s:%d: requireNotNull failed: '%s'\n", file, line, name);
        abort();
    }
    return ptr;
}
#define requireNotNull(ptr) requireNotNullFunction((void*)ptr, __FILE__, __LINE__, #ptr)
#define requireNotNullMessage(ptr, msg) requireNotNullFunction((void*)ptr, __FILE__, __LINE__, msg)

// Safe allocation macros - check for nullptr and abort with file/line info
static inline void *safeMallocFunction(size_t size, const char *file, int line) {
    if (size == 0)
        return nullptr;
    void *ret = malloc(size);
    if (!ret) {
        logError("FATAL: malloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return ret;
}
#define safeMalloc(size) safeMallocFunction(size, __FILE__, __LINE__)

static inline void *safeCallocFunction(size_t count, size_t size, const char *file, int line) {
    if (size == 0 || count == 0)
        return nullptr;
    void *ret = calloc(count, size);
    if (!ret) {
        logError("FATAL: calloc(%zu, %zu) failed at %s:%d\n", count, size, file, line);
        abort();
    }
    return ret;
}
#define safeCalloc(count, size) safeCallocFunction(count, size, __FILE__, __LINE__)

static inline void *safeReallocFunction(void *ptr, size_t size, const char *file, int line) {
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    void *ret = realloc(ptr, size);
    if (!ret) {
        logError("FATAL: realloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return ret;
}
#define safeRealloc(ptr, size) safeReallocFunction(ptr, size, __FILE__, __LINE__)

#ifdef PLATFORM_PS2

static inline void *safeMemalignFunction(size_t alignment, size_t size, const char *file, int line) {
    if (size == 0)
        return nullptr;
    void *ret = memalign(alignment, size);
    if (!ret) {
        logError("FATAL: memalign(%zu, %zu) failed at %s:%d\n", alignment, size, file, line);
        abort();
    }
    return ret;
}
#define safeMemalign(alignment, size) safeMemalignFunction(alignment, size, __FILE__, __LINE__)

#endif

// Reads exactly n bytes or aborts with the "pathForError" that caused the error.
static inline void safeFreadFunction(void *dst, size_t n, FILE *read_file, const char *pathForError, const char *file, int line) {
    if (fread(dst, 1, n, read_file) != n) {
        logError("FATAL: failed to read %zu bytes from %s at %s:%d\n", n, pathForError, file, line);
        abort();
    }
}
#define safeFread(dst, n, file, pathForError) safeFreadFunction(dst, n, file, pathForError, __FILE__, __LINE__)

static inline char *safeStrdupFunction(const char *str, const char *file, int line) {
    char *ret = strdup(str);
    if (!ret) {
        logError("FATAL: strdup() failed at %s:%d\n", file, line);
        abort();
    }
    return ret;
}
#define safeStrdup(str) safeStrdupFunction(str, __FILE__, __LINE__)

#define ZERO_STRUCT(s) memset(&(s), 0, sizeof(s))

// Truncates to 6 decimal places, matching the HTML5 runner's ClampFloat
static inline GMLReal clampFloat(GMLReal f) {
    return ((GMLReal) ((int64_t) (f * 1000000.0))) / 1000000.0;
}

#define BGR_B(c) (((c) >> 16) & 0xFF)
#define BGR_G(c) (((c) >>  8) & 0xFF)
#define BGR_R(c) (((c) >>  0) & 0xFF)
#define BGR_A(c) (((c) >> 24) & 0xFF)

// Mixes 2 colors with a blend factor
static inline int32_t Color_lerp(int32_t color1, int32_t color2, float blending) {
    int32_t r1 = BGR_R(color1), g1 = BGR_G(color1), b1 = BGR_B(color1);
    int32_t r2 = BGR_R(color2), g2 = BGR_G(color2), b2 = BGR_B(color2);
    float inv = 1.0f - blending;
    // Rounded, not truncated: merge_color(c_black, c_white, 0.5) is 0x808080 in GameMaker, while
    // truncation gave 0x7F7F7F -- every half-and-half blend came out one step dark per channel.
    // (Not to be confused with vertex alpha in floatToUnormByte, where the runtime does truncate.)
    int32_t r = (int32_t)((float) r2 * blending + (float) r1 * inv + 0.5f) & 0xFF;
    int32_t g = (int32_t)((float) g2 * blending + (float) g1 * inv + 0.5f) & 0xFF;
    int32_t b = (int32_t)((float) b2 * blending + (float) b1 * inv + 0.5f) & 0xFF;
    return r | (g << 8) | (b << 16);
}

static inline void bsGetDirname(char* path) {
    if (!path || *path == '\0') {
        return;
    }
    
    char* lastSlash = strrchr(path, '/');
    char* lastBackslash = strrchr(path, '\\');
    char* target = nullptr;
    if (lastSlash != nullptr && (target == nullptr || lastSlash > target))
        target = lastSlash;
    if (lastBackslash != nullptr && (target == nullptr || lastBackslash > target))
        target = lastBackslash;
#ifdef PLATFORM_VITA
    if (target == nullptr)
        target = strrchr(path, ':');
#endif

    if (target) {
        if (*target == ':') {
            *(target + 1) = '\0';
        } else if (target == path || *(target - 1) == ':') {
            *(target + 1) = '\0';
        } else {
            *target = '\0';
        }
    } else {
        path[0] = '.';
        path[1] = '\0';
    }
}

#define shcopyFromTo(src, dst)                        \
do {                                        \
(dst) = NULL;                           \
for (int i = 0; i < shlen(src); i++)    \
shput((dst), (src)[i].key, (src)[i].value); \
} while (0)

typedef struct {
    char* key;
    bool value;
} StringBooleanEntry;

#endif /* _BS_UTILS_H_ */
