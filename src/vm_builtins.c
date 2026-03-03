#include "vm_builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"
#include "utils.h"

// ===[ BUILTIN FUNCTION REGISTRY ]===
typedef struct {
    char* key;
    BuiltinFunc value;
} BuiltinEntry;

static bool initialized = false;
static BuiltinEntry* builtinMap = nullptr;

static void registerBuiltin(const char* name, BuiltinFunc func) {
    shput(builtinMap, (char*) name, func);
}

BuiltinFunc VMBuiltins_find(const char* name) {
    ptrdiff_t idx = shgeti(builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    return builtinMap[idx].value;
}

// ===[ BUILTIN IMPLEMENTATIONS ]===

static void logStubbedFunction(VMContext* ctx, const char* funcName) {
    // Log once per (callingCode, funcName) pair
    const char* callerName = VM_getCallerName(ctx);
    char* dedupKey = VM_createDedupKey(callerName, funcName);

    if (0 > shgeti(ctx->loggedStubbedFuncs, dedupKey)) {
        shput(ctx->loggedStubbedFuncs, dedupKey, true);
        fprintf(stderr, "VM: Stubbed function '%s' called from '%s'!\n", funcName, callerName);
    }
    free(dedupKey);
}

static RValue builtinShowDebugMessage(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) {
        fprintf(stderr, "[show_debug_message] Expected at least 1 argument\n");
        return (RValue){ .type = RVALUE_UNDEFINED };
    }

    char* val = RValue_toString(args[0]);
    printf("%s\n", val);
    free(val);

    return (RValue){ .type = RVALUE_UNDEFINED };
}

static RValue builtinStringLength(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount || args[0].type != RVALUE_STRING) {
        return (RValue){ .int32 = 0, .type = RVALUE_INT32 };
    }
    return (RValue){ .int32 = (int32_t) strlen(args[0].string), .type = RVALUE_INT32 };
}

static RValue builtinReal(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };

    switch (args[0].type) {
        case RVALUE_REAL:
            return args[0];
        case RVALUE_INT32:
        case RVALUE_BOOL:
            return (RValue){ .real = (double) args[0].int32, .type = RVALUE_REAL };
        case RVALUE_INT64:
            return (RValue){ .real = (double) args[0].int64, .type = RVALUE_REAL };
        case RVALUE_STRING:
            return (RValue){ .real = strtod(args[0].string, nullptr), .type = RVALUE_REAL };
        default:
            return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    }
}

static RValue builtinString(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .string = "", .type = RVALUE_STRING };

    char buf[64];
    char* result;
    switch (args[0].type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "%.16g", args[0].real);
            result = strdup(buf);
            return (RValue){ .string = result, .type = RVALUE_STRING, .ownsString = true };
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "%d", args[0].int32);
            result = strdup(buf);
            return (RValue){ .string = result, .type = RVALUE_STRING, .ownsString = true };
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "%lld", (long long) args[0].int64);
            result = strdup(buf);
            return (RValue){ .string = result, .type = RVALUE_STRING, .ownsString = true };
        case RVALUE_STRING:
            return args[0];
        case RVALUE_BOOL:
            return (RValue){ .string = args[0].int32 ? "1" : "0", .type = RVALUE_STRING };
        default:
            return (RValue){ .string = "undefined", .type = RVALUE_STRING };
    }
}

static RValue builtinFloor(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    return (RValue){ .real = floor(val), .type = RVALUE_REAL };
}

static RValue builtinCeil(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    return (RValue){ .real = ceil(val), .type = RVALUE_REAL };
}

static RValue builtinRound(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    return (RValue){ .real = round(val), .type = RVALUE_REAL };
}

static RValue builtinAbs(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    return (RValue){ .real = fabs(val), .type = RVALUE_REAL };
}

static RValue builtinSign(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    double result = (val > 0.0) ? 1.0 : ((0.0 > val) ? -1.0 : 0.0);
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

static RValue builtinMax(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double result = -INFINITY;
    repeat(argCount, i) {
        double val = RValue_toReal(args[i]);
        if (val > result) result = val;
    }
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

static RValue builtinMin(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double result = INFINITY;
    repeat(argCount, i) {
        double val = RValue_toReal(args[i]);
        if (result > val) result = val;
    }
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

static RValue builtinPower(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (2 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double base = RValue_toReal(args[0]);
    double exp = RValue_toReal(args[1]);
    return (RValue){ .real = pow(base, exp), .type = RVALUE_REAL };
}

static RValue builtinSqrt(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double val = RValue_toReal(args[0]);
    return (RValue){ .real = sqrt(val), .type = RVALUE_REAL };
}

static RValue builtinIsString(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double result = (args[0].type == RVALUE_STRING) ? 1.0 : 0.0;
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

static RValue builtinIsReal(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 0.0, .type = RVALUE_REAL };
    double result = (args[0].type == RVALUE_REAL || args[0].type == RVALUE_INT32 || args[0].type == RVALUE_INT64 || args[0].type == RVALUE_BOOL) ? 1.0 : 0.0;
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

static RValue builtinIsUndefined(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) ctx;
    if (1 > argCount) return (RValue){ .real = 1.0, .type = RVALUE_REAL };
    double result = (args[0].type == RVALUE_UNDEFINED) ? 1.0 : 0.0;
    return (RValue){ .real = result, .type = RVALUE_REAL };
}

// ===[ REGISTRATION ]===

void VMBuiltins_registerAll(void) {
    requireMessage(!initialized, "Attempting to register all VMBuiltins, but it was already registered!");
    initialized = true;

    registerBuiltin("show_debug_message", builtinShowDebugMessage);
    registerBuiltin("string_length", builtinStringLength);
    registerBuiltin("real", builtinReal);
    registerBuiltin("string", builtinString);
    registerBuiltin("floor", builtinFloor);
    registerBuiltin("ceil", builtinCeil);
    registerBuiltin("round", builtinRound);
    registerBuiltin("abs", builtinAbs);
    registerBuiltin("sign", builtinSign);
    registerBuiltin("max", builtinMax);
    registerBuiltin("min", builtinMin);
    registerBuiltin("power", builtinPower);
    registerBuiltin("sqrt", builtinSqrt);
    registerBuiltin("is_string", builtinIsString);
    registerBuiltin("is_real", builtinIsReal);
    registerBuiltin("is_undefined", builtinIsUndefined);
}
