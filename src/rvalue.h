#pragma once
#include <stdint.h>
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real_type.h"
#include "stb_ds.h"
#include "utils.h"

// ===[ RValue - Tagged Union ]===
typedef enum {
    RVALUE_REAL = 0,
    RVALUE_STRING = 1,
    RVALUE_INT32 = 2,
    RVALUE_INT64 = 3,
    RVALUE_BOOL = 4,
    RVALUE_UNDEFINED = 5,
    RVALUE_ARRAY_REF = 6,
} RValueType;

typedef struct {
    union {
        GMLReal real;
        int32_t int32;
#ifndef NO_RVALUE_INT64
        int64_t int64;
#endif
        const char* string;
    };
    // We use uint8_t for the type instead of RValueType because a enum value occupies 4 bytes, while uint8_t occupies 1 byte
    uint8_t type;
    bool ownsString;
} RValue;

static RValue RValue_makeReal(GMLReal val) {
    return (RValue){ .real = val, .type = RVALUE_REAL };
}

static RValue RValue_makeInt32(int32_t val) {
    return (RValue){ .int32 = val, .type = RVALUE_INT32 };
}

static RValue RValue_makeInt64(int64_t val) {
#ifdef NO_RVALUE_INT64
    // Values that don't fit in int32 get promoted to real instead of clamped, because clamping to INT32_MIN causes arithmetic overflow bugs
    // (example: Undertale's mercymod = -99999999999999 in the Asriel fight)
    if (val > INT32_MAX || INT32_MIN > val) {
        return (RValue){ .real = (GMLReal) val, .type = RVALUE_REAL };
    } else {
        return (RValue){ .int32 = (int32_t) val, .type = RVALUE_INT32 };
    }
#else
    return (RValue){ .int64 = val, .type = RVALUE_INT64 };
#endif
}

static RValue RValue_makeBool(bool val) {
    return (RValue){ .int32 = val ? 1 : 0, .type = RVALUE_BOOL };
}

static RValue RValue_makeString(const char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = false };
}

static RValue RValue_makeOwnedString(char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = true };
}

static RValue RValue_makeUndefined(void) {
    return (RValue){ .type = RVALUE_UNDEFINED };
}

// Creates an array reference that aliases another variable's array data.
// The sourceVarID identifies which variable's array map to use for reads/writes.
static RValue RValue_makeArrayRef(int32_t sourceVarID) {
    return (RValue){ .int32 = sourceVarID, .type = RVALUE_ARRAY_REF };
}

// Converts an RValue to a heap-allocated string representation.
// The caller must free the returned string
static char* RValue_toString(RValue val) {
    char buf[64];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "%.16g", val.real);
            return safeStrdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "%d", val.int32);
            return safeStrdup(buf);
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "%lld", (long long) val.int64);
            return safeStrdup(buf);
#endif
        case RVALUE_STRING:
            return safeStrdup(val.string != nullptr ? val.string : "");
        case RVALUE_BOOL:
            return safeStrdup(val.int32 ? "1" : "0");
        case RVALUE_UNDEFINED:
            return safeStrdup("undefined");
        case RVALUE_ARRAY_REF:
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            return safeStrdup(buf);
    }
    return safeStrdup("");
}

// Converts an RValue to a heap-allocated string representation, used for debug logs.
// The caller must free the returned string
static char* RValue_toStringFancy(RValue val) {
    switch (val.type) {
        case RVALUE_STRING: {
            char* valueAsString = RValue_toString(val);

            // length + quotes (2) + null terminator
            int newLength = strlen(valueAsString) + 3;
            char* valueWithQuotes = safeCalloc(newLength, sizeof(char));
            snprintf(valueWithQuotes, newLength, "\"%s\"", valueAsString);

            free(valueAsString);

            return valueWithQuotes;
        }
        default: {
            return RValue_toString(val);
        }
    }
}

// Converts an RValue to a heap-allocated string with a type tag prefix, used for trace-stack output.
// Examples: int32(42), real(3.14), "hello", bool(true), undefined, <array_ref:5>
// The caller must free the returned string
static char* RValue_toStringTyped(RValue val) {
    char buf[128];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "real(%.16g)", val.real);
            return safeStrdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "int32(%d)", val.int32);
            return safeStrdup(buf);
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "int64(%lld)", (long long) val.int64);
            return safeStrdup(buf);
#endif
        case RVALUE_STRING: {
            const char* str = val.string != nullptr ? val.string : "";
            size_t needed = strlen(str) + 3;
            char* result = safeCalloc(needed, sizeof(char));
            snprintf(result, needed, "\"%s\"", str);
            return result;
        }
        case RVALUE_BOOL:
            return safeStrdup(val.int32 ? "bool(true)" : "bool(false)");
        case RVALUE_UNDEFINED:
            return safeStrdup("undefined");
        case RVALUE_ARRAY_REF:
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            return safeStrdup(buf);
    }
    return safeStrdup("???");
}

static void RValue_free(RValue* val) {
    if (val->type == RVALUE_STRING && val->ownsString && val->string != nullptr) {
        free((void*) val->string);
        val->string = nullptr;
        val->ownsString = false;
    }
}

static GMLReal RValue_toReal(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real;
        case RVALUE_INT32:  return (GMLReal) val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return (GMLReal) val.int64;
#endif
        case RVALUE_BOOL:   return (GMLReal) val.int32;
        case RVALUE_STRING: return GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY_REF: return 0.0;
        default:            return 0.0;
    }
}

static int32_t RValue_toInt32(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int32_t) val.real;
        case RVALUE_INT32:  return val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return (int32_t) val.int64;
#endif
        case RVALUE_BOOL:   return val.int32;
        case RVALUE_STRING: return (int32_t) GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY_REF: return 0;
        default:            return 0;
    }
}

static int64_t RValue_toInt64(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int64_t) val.real;
        case RVALUE_INT32:  return (int64_t) val.int32;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return val.int64;
#endif
        case RVALUE_BOOL:   return (int64_t) val.int32;
        case RVALUE_STRING: return (int64_t) GMLReal_strtod(val.string, nullptr);
        case RVALUE_ARRAY_REF: return 0;
        default:            return 0;
    }
}

static bool RValue_toBool(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real > 0.5;
        case RVALUE_INT32:  return val.int32 > 0;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:  return val.int64 > 0;
#endif
        case RVALUE_BOOL:   return val.int32 != 0;
        case RVALUE_STRING: return val.string != nullptr && val.string[0] != '\0';
        case RVALUE_ARRAY_REF: return false;
        default:            return false;
    }
}

// ===[ ArrayMapEntry - used by all array variable storage ]===
typedef struct {
    int64_t key;
    RValue value;
} ArrayMapEntry;

static void RValue_freeAllRValuesInMap(ArrayMapEntry* map) {
    repeat(hmlen(map), i) {
        RValue_free(&map[i].value);
    }
}