#ifndef _BS_VM_H_
#define _BS_VM_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>

#include "data_win.h"
#include "rvalue.h"
#include "utils.h"
#include "profiler.h"
#include "int_int_hashmap.h"
#include "string_builder.h"

// ===[ Instance Types (signed 16-bit) ]===
#define INSTANCE_SELF      (-1)
#define INSTANCE_OTHER     (-2)
#define INSTANCE_ALL       (-3)
#define INSTANCE_NOONE     (-4)
#define INSTANCE_GLOBAL    (-5)
#define INSTANCE_BUILTIN   (-6)
#define INSTANCE_LOCAL     (-7)
#define INSTANCE_STACKTOP  (-9)
#define INSTANCE_ARG       (-15)
#define INSTANCE_STATIC    (-16)

// ===[ Variable Types (upper 5 bits of varRef, extracted with (varRef >> 24) & 0xF8) ]===
#define VARTYPE_ARRAY     0x00
#define VARTYPE_STACKTOP  0x80
#define VARTYPE_NORMAL    0xA0
#define VARTYPE_INSTANCE  0xE0

// ===[ Variable Identities ]===
#define VARIABLE_BUILTIN (-6)

// ===[ Room Constants ]===
#define ROOM_RESTARTGAME (-200) // The reason why it is -200 is because the GameMaker-HTML5 runner uses -200 too (see Globals.js)

// ===[ GML Math Epsilon (used for floating-point comparisons) ]===
// The real GameMaker runner uses epsilon-based comparison for all numeric CMP operations.
// Default value matches the HTML5 runner's g_GMLMathEpsilon (1e-5 for double precision).
// When using single-precision floats, we use 1e-4 to work around accumulated rounding errors from
// non-IEEE FPUs (example: PS2's R5900 which rounds toward zero instead of round-to-nearest) can
// exceed the default epsilon.
#ifdef USE_FLOAT_REALS
#define GML_MATH_EPSILON 1e-4
#else
#define GML_MATH_EPSILON 1e-5
#endif

// GMS 1.4 supports up to 16 arguments per script call
// Newer GM versions do NOT have an argument limit however, but the limit is still used for the old classic style "argumentX" access
#define GML_MAX_ARGUMENTS 16

#define INSTANCE_ID_BASE 100000

// ===[ Comparison Kinds ]===
#define CMP_LT  1
#define CMP_LTE 2
#define CMP_EQ  3
#define CMP_NEQ 4
#define CMP_GTE 5
#define CMP_GT  6

// ===[ Opcodes ]===
#define OP_CONV     0x07
#define OP_MUL      0x08
#define OP_DIV      0x09
#define OP_REM      0x0A
#define OP_MOD      0x0B
#define OP_ADD      0x0C
#define OP_SUB      0x0D
#define OP_AND      0x0E
#define OP_OR       0x0F
#define OP_XOR      0x10
#define OP_NEG      0x11
#define OP_NOT      0x12
#define OP_SHL      0x13
#define OP_SHR      0x14
#define OP_CMP      0x15
#define OP_POP      0x45
#define OP_PUSHI    0x84
#define OP_DUP      0x86
#define OP_CALLV    0x99
#define OP_RET      0x9C
#define OP_EXIT     0x9D
#define OP_POPZ     0x9E
#define OP_B        0xB6
#define OP_BT       0xB7
#define OP_BF       0xB8
#define OP_PUSHENV  0xBA
#define OP_POPENV   0xBB
#define OP_PUSH     0xC0
#define OP_PUSHLOC  0xC1
#define OP_PUSHGLB  0xC2
#define OP_PUSHBLTN 0xC3
#define OP_CALL     0xD9
#define OP_BREAK    0xFF

// ===[ Extended BREAK Sub-Opcodes (WAD version 17+) ]===
// Encoded in bits 0-15 of the BREAK instruction (instrInstanceType field, as int16_t)
#define BREAK_CHKINDEX     (-1)  // Validate array index bounds
#define BREAK_PUSHAF       (-2)  // Pop array ref + index, push element (final dimension)
#define BREAK_POPAF        (-3)  // Pop value + array ref + index, store at index
#define BREAK_PUSHAC       (-4)  // Pop array ref + index, push sub-array ref (intermediate dimension)
#define BREAK_SETOWNER     (-5)  // Pop and discard (copy-on-write owner tracking)
#define BREAK_ISSTATICOK   (-6)  // Push bool: has static init already run for this function?
#define BREAK_SETSTATIC    (-7)  // Mark current function's static as initialized
#define BREAK_SAVEAREF     (-8)  // Save top-of-stack array ref for compound assignment
#define BREAK_RESTOREAREF  (-9)  // Push previously saved array ref
#define BREAK_ISNULLISH    (-10) // Pop value, push bool: is the value nullish (undefined / pointer_null)?
#define BREAK_PUSHREF      (-11) // Push an asset reference (or a script/function reference) encoded in the 32-bit operand

// Max amount of args a function call can have until the args are heap-alloced.
#define VM_MAX_STACK_ARGS 16

// ===[ Variable Types for V17 Array Access ]===
#define VARTYPE_ARRAYPUSHAF 0x10  // Push array reference (read context)
#define VARTYPE_ARRAYPOPAF  0x90  // Push array reference (write context)

// ===[ FuncCallCache - Cached resolution for CALL instructions ]===
// Avoids per-call string hash lookups in both the builtin map and funcMap.
// Resolved once during VM_create, then used directly by handleCall.
typedef struct {
    void* builtin; // cached BuiltinFunc pointer, or nullptr
    int32_t scriptCodeIndex; // cached script code index, or -1 if not a script
} FuncCallCache;

// ===[ CallFrame - Saved state for script-to-script calls ]===
typedef struct CallFrame {
    uint32_t savedIP;
    uint32_t savedCodeEnd;
    uint8_t* savedBytecodeBase;
    RValue* savedLocals;
    uint32_t savedLocalsCount;
    const char* savedCodeName;
    int32_t savedSavearefBalance;
    IntIntHashMap* savedCodeLocalsSlotMap;
    RValue* savedScriptArgs;
    int32_t savedScriptArgCount;
    int32_t savedCurrentCodeIndex;
    struct CallFrame* parent;
} CallFrame;

// ===[ EnvFrame - Saved context for with-statement (PushEnv/PopEnv) ]===
typedef struct EnvFrame {
    struct Instance* savedInstance;
    struct Instance* savedOtherInstance; // Saved otherInstance to restore on PopEnv
    struct Instance** instanceList; // stb_ds array of matching instances (nullptr for single-instance)
    int32_t currentIndex;           // Current position in instanceList
    struct EnvFrame* parent;
} EnvFrame;

typedef struct {
    int32_t jumpToOnException;
    int32_t jumpToOnSuccess;
    int32_t boundToCallDepth;
    int32_t stackTop;
} ExceptionHandlerFrame;

// ===[ VMStack - Upward-growing array of RValue slots ]===
#define VM_STACK_SIZE 1024
#define VM_EXCEPTION_HANDLER_FRAME_STACK_SIZE 16

typedef struct {
    int32_t top;
    RValue slots[VM_STACK_SIZE];
} VMStack;

// Forward declarations
struct Runner;
#ifndef VM_CONTEXT_DEFINED
#define VM_CONTEXT_DEFINED
typedef struct VMContext VMContext;
#endif

// ===[ Builtin Functions Manager ]===
#ifndef BUILTINFUNC_DEFINED
#define BUILTINFUNC_DEFINED
typedef RValue (*BuiltinFunc)(VMContext* ctx, RValue* args, int32_t argCount);
#endif

typedef struct {
    char* key;
    BuiltinFunc value;
} BuiltinEntry;

typedef struct {
    char* message;
} VMException;

typedef struct { char* key; int32_t value; } CodeIndexByNameEntry;
typedef struct { char* key; CodeLocals* value; } CodeLocalsMapEntry;
typedef struct { int32_t key; int32_t* value; } CrossRefMapEntry;

// ===[ VMContext - Holds all VM state ]===
// Fields are ordered by access frequency so that the hottest data sits in the first bytes of the struct
// This way data can be kept "hot" in the CPU cache or, depending on the platform, in scratchpad RAM
struct VMContext {
    // Hot: touched every instruction in the dispatch loop
    uint8_t* bytecodeBase;
    uint32_t ip;
    uint32_t codeEnd;
    RValue* localVars;
    uint32_t localVarCount;
    struct Instance* globalScopeInstance; // used when GLOB scripts are being executed, and used for the "global" reference
    struct Instance* currentInstance;
    struct Instance* otherInstance; // "other" instance for collision events
    DataWin* dataWin;
    struct Runner* runner;
    // BC17+: varID -> localVars slot lookup for the current code. Points into codeLocalsSlotMaps[currentCodeIndex] for BC17+, nullptr for BC16.
    IntIntHashMap* currentCodeLocalsSlotMap;
    FuncCallCache* funcCallCache;
    const char* currentCodeName;
    int32_t currentCodeIndex; // Index into code.entries for the currently executing code

    // Warm: touched on calls, variable resolution, event dispatch
    CallFrame* callStack;
    int32_t callDepth;
    EnvFrame* envStack; // Environment stack for with-statements (PushEnv/PopEnv)
    RValue* scriptArgs;       // Arguments passed to current script (nullptr for non-script code)
    int32_t scriptArgCount;   // Number of arguments passed
    int32_t selfId;
    int32_t otherId;
    // Current event context (set by Runner_executeEvent, -1 when not in an event)
    int32_t currentEventType;
    int32_t currentEventSubtype;
    int32_t currentEventObjectIndex; // objectIndex of the object that owns the executing event handler
    // Cached varID for the built-in "creator" self variable (-1 if not found)
    int32_t creatorVarID;
    uint32_t funcCallCacheCount;
    bool traceEventInherited;
    bool hasFixedSeed;
    bool actionRelativeFlag; // D&D action relative flag (set by action_set_relative)

    // V17+ extended BREAK opcode state
    bool* staticInitialized; // Per-code-entry flag for isstaticok/setstatic (allocated in VM_create)
    // Static variables: per-constructor shared static struct.
    Instance** staticStructs;
    // BC17+: owner token set by BREAK_SETOWNER. Arrays whose .owner mismatches fork on write.
    void* currentArrayOwner;
    // SAVEAREF/RESTOREAREF balance tracker.
    int32_t savearefBalance;
#ifdef ENABLE_WAD17
    VMException* exception;
    VMException* parkedException;
    int32_t exceptionHandlerStackTop;
    ExceptionHandlerFrame exceptionHandlerFrameStack[VM_EXCEPTION_HANDLER_FRAME_STACK_SIZE];
#endif

    // Cold: init-only or rare lookups
    BuiltinEntry* builtinMap;
    bool registeredBuiltinFunctions;
    // funcName -> codeIndex hash map (stb_ds)
    CodeIndexByNameEntry* codeIndexByName;
    // codeName -> CodeLocals* hash map (stb_ds)
    CodeLocalsMapEntry* codeLocalsMap;
    // BC13/BC14/BC17+: A map of CODE indexes -> localVars slot lookup map
    IntIntHashMap* codeLocalsSlotMaps;
    // varName -> varID hash map for self/instance-scoped variables (stb_ds).
    struct { char* key; int32_t value; }* varNameMap;
    int32_t nextDynamicVarID;
    // "codeName\tfuncName" -> true, for deduplicating unknown function warnings
    StringBooleanEntry* loggedUnknownFuncs;
    // "codeName\tfuncName" -> true, for deduplicating stubbed function warnings
    StringBooleanEntry* loggedStubbedFuncs;
    // Cross-reference map for disassembler: targetCodeIndex -> stb_ds array of callerCodeIndex
    CrossRefMapEntry* crossRefMap;
    bool alwaysLogUnknownFunctions;
#ifdef ENABLE_VM_STUB_LOGS
    bool alwaysLogStubbedFunctions;
#endif
#ifdef ENABLE_VM_TRACING
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* collisionsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* tilesToBeTraced;
    // Minimum frameCount before opcode/stack traces are emitted (default 0)
    int traceBytecodeAfterFrame;
#endif
    Profiler* profiler;

#ifdef ENABLE_VM_OPCODE_PROFILER
    bool opcodeProfilerEnabled;
    uint64_t opcodeCounts[256];
    // Per-opcode breakdown by (type1, type2). Heap-allocated when the profiler is enabled (512 KB), nullptr otherwise.
    // Indexed as opcodeVariantCounts[opcode * 256 + type1 * 16 + type2].
    uint64_t* opcodeVariantCounts;
    // BREAK (0xFF) sub-opcode counts. Indexed by -breakType (so -1 -> [1], -9 -> [9]). Size 64 covers all currently defined sub-ops with room to spare.
    uint64_t breakSubOpCounts[64];
    // Per-opcode breakdown by actual runtime RValue types (typeA, typeB) for arithmetic/comparison/conversion ops.
    // Indexed as opcodeRValueTypeCounts[opcode * 256 + typeA * 16 + typeB]. typeB = 0xF for unary ops. 512 KB heap-allocated.
    uint64_t* opcodeRValueTypeCounts;
#endif

    // Stack at the end because it is a big chunky boi (we don't want it pushing fields around)
    VMStack stack;
};

// ===[ Public API ]===
Instance* VM_findInstanceByTarget(VMContext* ctx, int32_t target);
VMContext* VM_create(DataWin* dataWin);
void VM_reset(VMContext* ctx);
RValue VM_executeCode(VMContext* ctx, int32_t codeIndex);
RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount);
void VM_free(VMContext* ctx);
bool VM_isObjectOrDescendant(DataWin* dataWin, int32_t objectIndex, int32_t targetObjectIndex);
int32_t VM_resolveInstanceTarget(VMContext* ctx, int32_t target);
void VM_buildCrossReferences(VMContext* ctx);
void VM_disassemble(VMContext* ctx, int32_t codeIndex);
#ifdef ENABLE_VM_OPCODE_PROFILER
// Prints a sorted summary of opcode execution counts to stderr. Does nothing if the opcode profiler was never enabled.
void VM_printOpcodeProfilerReport(const VMContext* ctx);
#endif
void VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func);
#if defined(_MSC_VER) && !defined(__clang__)
// Some versions of MSVC complain that the functions aren't the right type because of struct bullshit.
// We guard this behind _MSC_VER to keep type checking on other compilers to find legitimate type mismatch errors.
#define VM_registerBuiltin(ctx,name,func) VM_registerBuiltin(ctx,name,(BuiltinFunc)func)
#endif
BuiltinFunc VM_findBuiltin(VMContext* ctx, const char* name);

char* VM_getVariableNameByVarId(VMContext* ctx, int32_t varId);
RValue VM_structGetVariableByVarId(Instance* structInst, int32_t varId, int32_t arrayIndex);
RValue VM_structGetVariableByVarName(VMContext* ctx, Instance* structInst, const char* name, int32_t arrayIndex);
// Set a named field on a freshly-built GML struct.
void VM_structSet(VMContext* ctx, Instance* structInst, const char* name, RValue val, int32_t arrayIndex);
void VM_structSetAndFreeVal(VMContext* ctx, Instance* structInst, const char* name, RValue val, int32_t arrayIndex);

// Create or reuse the shared static struct for a constructor code entry.
Instance* VM_getOrCreateStaticStruct(VMContext* ctx, int32_t codeIndex);

// @@CopyStatic@@: chain the current constructor's static struct to a parent constructor's static struct (static inheritance).
void VM_copyStatic(VMContext* ctx, RValue* parentRef);

// Look up the varID for a self-scoped variable name, allocating a fresh synthetic ID if absent.
int32_t VM_getOrAllocateVarID(VMContext* ctx, const char* name);

// Writes to the VMContext's scriptArgs, resizing the underlying array if needed
// The "val" will be RValue_makeIndependent(val), it won't be freed
void VM_writeToScriptArgs(VMContext* ctx, int32_t writeIndex, RValue val);
void VM_writeToScriptArgsArrayElement(VMContext* ctx, int32_t writeIndex, int32_t arrayIndex, RValue val);

static inline const char* VM_getCallerName(VMContext* ctx) {
    return ctx->currentCodeName != nullptr ? ctx->currentCodeName : "<unknown>";
}

static inline char* VM_createDedupKey(const char* callerName, const char* funcName) {
    // Build dedup key: "callerName\tfuncName"
    size_t keyLen = strlen(callerName) + 1 + strlen(funcName) + 1;
    char* dedupKey = (char *)safeMalloc(keyLen);
    snprintf(dedupKey, keyLen, "%s\t%s", callerName, funcName);
    return dedupKey;
}

// ===[ Trace Helpers ]===

#ifdef ENABLE_VM_TRACING
/**
 * @brief Checks if a variable access should be traced.
 *
 * Matches the trace map entries in order: wildcard "*", bare scope name (e.g. "obj_player" or "global"),
 * alternate scope name (e.g. "self" for any instance), or qualified "scope.var" format
 * (e.g. "obj_player.x", "global.hp", "self.x"). Short-circuits before formatting
 * the qualified name when possible.
 *
 * @param traceMap The string-boolean hash map of trace filters (from --trace-variable-reads/writes).
 * @param scopeName The scope of the variable: an object name (e.g. "obj_player") or "global".
 * @param altScopeName An alternate scope name to also match (e.g. "self" for instance variables), or nullptr.
 * @param varName The variable name being accessed (e.g. "x").
 * @return true if the access matches a trace filter and should be logged.
 */
static inline bool VM_shouldTraceVariable(StringBooleanEntry* traceMap, const char* scopeName, const char* altScopeName, const char* varName) {
    if (shlen(traceMap) == 0) return false;
    // "*" should trace EVERYTHING
    if (shgeti(traceMap, "*") != -1) return true;
    // "obj_mainchara" should trace EVERY variable read/write to that object
    if (shgeti(traceMap, scopeName) != -1) return true;
    // "self" should trace EVERY "self" scope variable read/write to ALL objects
    if (altScopeName != nullptr && shgeti(traceMap, altScopeName) != -1) return true;
    // "hp" should trace EVERY "hp" variable read/write to ALL objects
    if (shgeti(traceMap, varName) != -1) return true;
    // "obj_mainchara.hp" should trace EVERY variable read/write to the "hp" variable on the "obj_mainchara" object.
    size_t formattedSize = strlen(scopeName) + 1 + strlen(varName) + 1;
    char *formatted = (char *)safeMalloc(formattedSize);
    snprintf(formatted, formattedSize, "%s.%s", scopeName, varName);
    if (shgeti(traceMap, formatted) != -1) {
        free(formatted);
        return true;
    }
    free(formatted);
    if (altScopeName != nullptr) {
        size_t altFormattedSize = strlen(altScopeName) + 1 + strlen(varName) + 1;
        char *altFormatted = (char *)safeMalloc(altFormattedSize);
        snprintf(altFormatted, altFormattedSize, "%s.%s", altScopeName, varName);
        if (shgeti(traceMap, altFormatted) != -1) {
            free(altFormatted);
            return true;
        }
        free(altFormatted);
    }
    return false;
}

static inline void VM_checkIfVariableShouldBeTracedAndLog(VMContext* ctx, const char* scopeName, const char* altScopeName, const char* name, RValue value, bool isWrite, int32_t arrayIndex, int32_t instanceId, const char* additional) {
    StringBooleanEntry* varModificationsToBeTraced = isWrite ? ctx->varWritesToBeTraced : ctx->varReadsToBeTraced;
    if (!VM_shouldTraceVariable(varModificationsToBeTraced, scopeName, altScopeName, name))
        return;

    char* rvalueAsString = RValue_toStringTyped(value);
    const char* verb = isWrite ? "WRITE" : "READ";
    const char* arrow = isWrite ? "=" : "->";
    char indexBuf[16] = "";
    if (arrayIndex >= 0) snprintf(indexBuf, sizeof(indexBuf), "[%d]", arrayIndex);
    char instanceIdBuf[28] = "";
    if (instanceId >= 0) snprintf(instanceIdBuf, sizeof(instanceIdBuf), " (instanceId=%d)", instanceId);
    logInfo("VM: [%s] %s %s.%s%s %s %s%s%s\n", ctx->currentCodeName, verb, scopeName, name, indexBuf, arrow, rvalueAsString, instanceIdBuf, additional);
    free(rvalueAsString);
}
#endif

#endif /* _BS_VM_H_ */
