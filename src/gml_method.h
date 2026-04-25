#pragma once
#include <stdint.h>

// Forward declarations
typedef struct VMContext VMContext;
typedef struct RValue RValue;
typedef RValue (*BuiltinFunc)(VMContext* ctx, RValue* args, int32_t argCount);

// ===[ GMLMethod - Refcounted method binding ]===
typedef struct GMLMethod {
    int32_t refCount;
    int32_t codeIndex;
    int32_t boundInstanceId;
    // When non-null, this method refers to a built-in function rather than a script's code entry.
    BuiltinFunc builtin;
    // Original name for diagnostics when the method is an unresolved function reference (codeIndex=-1 and builtin=nullptr).
    const char* unresolvedName;
} GMLMethod;

GMLMethod* GMLMethod_create(int32_t codeIndex, int32_t boundInstanceId);
GMLMethod* GMLMethod_createBuiltin(BuiltinFunc builtin, int32_t boundInstanceId);
GMLMethod* GMLMethod_createUnresolved(const char* name, int32_t boundInstanceId);
void GMLMethod_incRef(GMLMethod* m);
// Decrement refCount. If it reaches 0, frees the struct. Safe on nullptr.
void GMLMethod_decRef(GMLMethod* m);
