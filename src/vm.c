#include "vm.h"
#include "vm_builtins.h"
#include "instance.h"
#include "runner.h"
#include "binary_utils.h"
#include "utils.h"
#include "bytecode_versions.h"
#include "profiler.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"

// Maximum number of local variables per code entry (stack-allocated arrays in VM_executeCode/VM_callCodeIndex)
#define MAX_CODE_LOCALS 128

// ===[ Stack Operations ]===

#ifdef ENABLE_VM_TRACING
static bool shouldTraceStack(VMContext* ctx) {
    if (shlen(ctx->stackToBeTraced) == 0) return false;
    if (ctx->traceBytecodeAfterFrame > ctx->runner->frameCount) return false;
    return shgeti(ctx->stackToBeTraced, "*") != -1 || shgeti(ctx->stackToBeTraced, ctx->currentCodeName) != -1;
}

// Returns a heap-allocated "[elem0, elem1, ..., elemN]" string for the current stack contents (bottom -> top). Caller frees.
static char* formatStackContents(VMContext* ctx) {
    StringBuilder sb = StringBuilder_create(256);
    StringBuilder_appendChar(&sb, '[');
    repeat(ctx->stack.top, si) {
        char* typed = RValue_toStringTyped(ctx->stack.slots[si]);
        if (si > 0) StringBuilder_append(&sb, ", ");
        StringBuilder_append(&sb, typed);
        free(typed);
    }
    StringBuilder_appendChar(&sb, ']');
    char* result = StringBuilder_toString(&sb);
    StringBuilder_free(&sb);
    return result;
}
#endif

#if IS_BC17_OR_HIGHER_ENABLED
// Returns the native byte size of a GML data type on the runner's stack.
// This is needed because the Dup instruction encodes byte counts, not slot counts.
// Only used by BC17+ Dup paths; BC16 Dup decodes the operand as a slot count directly.
static int gmlTypeNativeSize(uint8_t gmlType) {
    switch (gmlType) {
        case GML_TYPE_DOUBLE:   return 8;
        case GML_TYPE_INT32:    return 4;
        case GML_TYPE_INT64:    return 8;
        case GML_TYPE_BOOL:     return 4;
        case GML_TYPE_VARIABLE: return 16;
        case GML_TYPE_STRING:   return 4;
        case GML_TYPE_INT16:    return 4;
        default:                return 16;
    }
}
#endif

static void stackPush(VMContext* ctx, RValue val) {
    require(VM_STACK_SIZE > ctx->stack.top);
#ifdef ENABLE_VM_TRACING
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        ctx->stack.slots[ctx->stack.top++] = val;
        char* stackBuf = formatStackContents(ctx);
        fprintf(stderr, "VM: [%s] PUSH %s [stack=%d -> %d] %s\n", ctx->currentCodeName, valStr, ctx->stack.top - 1, ctx->stack.top, stackBuf);
        free(stackBuf);
        free(valStr);
        return;
    }
#endif
    ctx->stack.slots[ctx->stack.top++] = val;
}

#if IS_BC17_OR_HIGHER_ENABLED
static void stackPushTyped(VMContext* ctx, RValue val, uint8_t gmlStackType) {
    if (IS_BC17_OR_HIGHER(ctx)) {
        val.gmlStackType = gmlStackType;
    }
    stackPush(ctx, val);
}
#else
// BC16-only builds don't carry per-slot GML stack type, so this is just a plain push.
// Defined as a macro so the gmlStackType argument (often `instrType2(instr)`) is never computed at call sites.
#define stackPushTyped(ctx, val, gmlStackType) stackPush((ctx), (val))
#endif

static RValue stackPop(VMContext* ctx) {
    require(ctx->stack.top > 0);
    RValue val = ctx->stack.slots[--ctx->stack.top];
#ifdef ENABLE_VM_TRACING
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        char* stackBuf = formatStackContents(ctx);
        fprintf(stderr, "VM: [%s] POP  %s [stack=%d -> %d] %s\n", ctx->currentCodeName, valStr, ctx->stack.top + 1, ctx->stack.top, stackBuf);
        free(stackBuf);
        free(valStr);
    }
#endif
    return val;
}

// Helper function that calls stackPop and returns the result as an int32_t
static int32_t stackPopInt32(VMContext* ctx) {
    RValue rvalue = stackPop(ctx);
    int32_t value = RValue_toInt32(rvalue);
    RValue_free(&rvalue);
    return value;
}

static RValue* stackPeek(VMContext* ctx) {
    require(ctx->stack.top > 0);
    return &ctx->stack.slots[ctx->stack.top - 1];
}

// ===[ Instruction Decoding ]===

static uint8_t instrOpcode(uint32_t instr) {
    return (instr >> 24) & 0xFF;
}

static uint8_t instrType1(uint32_t instr) {
    return (instr >> 16) & 0xF;
}

static uint8_t instrType2(uint32_t instr) {
    return (instr >> 20) & 0xF;
}

static int16_t instrInstanceType(uint32_t instr) {
    return (int16_t) (instr & 0xFFFF);
}

static uint8_t instrCmpKind(uint32_t instr) {
    return (instr >> 8) & 0xFF;
}

static bool instrHasExtraData(uint32_t instr) {
    return (instr & 0x40000000) != 0;
}

// Jump offset for branch instructions: sign-extend 23 bits, multiply by 4
static int32_t instrJumpOffset(uint32_t instr) {
    return ((int32_t) (instr << 9)) >> 7;
}

static uint32_t extraDataSize(uint8_t type1) {
    switch (type1) {
        case GML_TYPE_DOUBLE: return 8;
        case GML_TYPE_INT64:  return 8;
        case GML_TYPE_FLOAT:  return 4;
        case GML_TYPE_INT32:  return 4;
        case GML_TYPE_BOOL:   return 4;
        case GML_TYPE_VARIABLE: return 4;
        case GML_TYPE_STRING: return 4;
        case GML_TYPE_INT16:  return 0;
        default:              return 0;
    }
}

// ===[ Reference Chain Resolution ]===

// Walks reference chains from the bytecode buffer and builds hash maps
// mapping absolute file offsets to resolved operand values.
// The bytecode buffer stays completely read-only.
// Patches bytecode operands in-place so that variable/function reference chain deltas
// are replaced with resolved indices. This avoids needing hash map lookups at runtime.
static void patchReferenceOperands(VMContext* ctx) {
    DataWin* dataWin = ctx->dataWin;
    uint8_t* buf = dataWin->bytecodeBuffer;
    size_t base = dataWin->bytecodeBufferBase;

    // Patch variable operands: replace delta with varIdx (preserving upper 5 bits)
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* v = &dataWin->vari.variables[varIdx];
        if (v->occurrences == 0) continue;

        uint32_t addr = v->firstAddress;
        repeat(v->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = BinaryUtils_readUint32(&buf[operandAddr - base]);
            uint32_t delta = operand & 0x07FFFFFF;
            uint32_t upperBits = operand & 0xF8000000;

            // Patch in-place: upper bits preserved, lower 27 = varIdx
            BinaryUtils_writeUint32(&buf[operandAddr - base], upperBits | (varIdx & 0x07FFFFFF));

            if (v->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }

    // Patch function operands: replace delta with funcIdx
    repeat(dataWin->func.functionCount, funcIdx) {
        Function* f = &dataWin->func.functions[funcIdx];
        if (f->occurrences == 0) continue;

        uint32_t addr = f->firstAddress;
        repeat(f->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = BinaryUtils_readUint32(&buf[operandAddr - base]);
            uint32_t delta = operand & 0x07FFFFFF;

            // Patch in-place: store funcIdx directly
            BinaryUtils_writeUint32(&buf[operandAddr - base], funcIdx);

            if (f->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }
}

// Resolve a variable operand: returns upper bits | varIndex (read directly from patched bytecode)
static uint32_t resolveVarOperand(const uint8_t* extraData) {
    return BinaryUtils_readUint32Aligned(extraData);
}

// Resolve a function operand: returns funcIndex (read directly from patched bytecode)
static uint32_t resolveFuncOperand(const uint8_t* extraData) {
    return BinaryUtils_readUint32Aligned(extraData);
}

// ===[ Array Operations ]===
//
// All arrays live as RVALUE_ARRAY (GMLArray*) inside a scalar variable slot (self vars, global vars, or local vars).
// Variable reads return the RValue (which may be an array pointer) and variable writes update the slot directly.
//
// Reads return a weak view of the slot value - callers must incRef + set ownsReference if they want to retain it.
//
// Writes (VARTYPE_ARRAY Pop, BREAK_POPAF, BREAK_PUSHAC materialisation) go through VM_arrayWriteAt,
// which handles:
//   * slot-not-yet-an-array -> allocate a fresh GMLArray
//   * CoW fork when another scope/slot owns the array (BC16 predicate uses the slot address; BC17+ predicate compares against ctx->currentArrayOwner set by BREAK_SETOWNER)
//   * grow-on-write past the current length
//   * transfer ownership of "val" into arr->data[index], freeing whatever was there before.
//
// Forward declarations
static Instance* findInstanceByTarget(VMContext* ctx, int32_t target);

// Read array[index]. Returns RVALUE_UNDEFINED when slot is not an array or when index is out of bounds.
// The returned RValue is a weak view, callers that stash it must strengthen (incRef, strdup).
static RValue VM_arrayReadAt(RValue* slot, int32_t index) {
    if (slot == nullptr || slot->type != RVALUE_ARRAY || slot->array == nullptr) {
        return (RValue){ .type = RVALUE_UNDEFINED };
    }
    RValue* cell = GMLArray_slot(slot->array, index);
    if (cell == nullptr) {
        return (RValue){ .type = RVALUE_UNDEFINED };
    }
    RValue result = *cell;
    result.ownsReference = false;
    return result;
}

// Copies "val" into *slot: dup string buffers, incRef arrays. Caller retains "val".
static void storeIntoArraySlot(RValue* slot, RValue val) {
    // Free whatever was there (decRefs owned arrays, frees owned strings).
    RValue_free(slot);
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        *slot = RValue_makeOwnedString(safeStrdup(val.string));
    } else if (val.type == RVALUE_ARRAY && val.array != nullptr) {
        GMLArray_incRef(val.array);
        val.ownsReference = true;
        *slot = val;
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (val.type == RVALUE_METHOD && val.method != nullptr) {
        GMLMethod_incRef(val.method);
        val.ownsReference = true;
        *slot = val;
#endif
    } else if (val.type == RVALUE_STRUCT && val.structInst != nullptr) {
        Instance_structIncRef(val.structInst);
        val.ownsReference = true;
        *slot = val;
    } else {
        val.ownsReference = false;
        *slot = val;
    }
}

// Write array[index] = val with CoW semantics. Always makes an independent copy of val, caller retains ownership and must RValue_free(&val) when done.
// `slot` is the RValue* holding the array (e.g. &globalVars[id], &inst->selfVars[..].value, &localVars[slot]).
// Returns the (possibly newly-forked) GMLArray* now in *slot.
static GMLArray* VM_arrayWriteAt(VMContext* ctx, RValue* slot, int32_t index, RValue val) {
    require(slot != nullptr);
    requireMessageFormatted(index >= 0, "Trying to write to an array using a negative index! Index: %d", index);

    void* intendedOwner;
#if IS_BC17_OR_HIGHER_ENABLED
    intendedOwner = IS_BC17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
#else
    intendedOwner = (void*) slot;
#endif

    // Case 1: slot doesn't hold an array yet, replace whatever's there with a fresh one.
    if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
        RValue_free(slot);
        GMLArray* fresh = GMLArray_create(0);
        fresh->owner = intendedOwner;
        *slot = RValue_makeArray(fresh);
        GMLArray_growTo(fresh, index + 1);
        storeIntoArraySlot(GMLArray_slot(fresh, index), val);
        return fresh;
    }

    GMLArray* arr = slot->array;

    // Case 2: CoW fork check.
    bool needFork;
#if IS_BC17_OR_HIGHER_ENABLED
    if (IS_BC17_OR_HIGHER(ctx)) {
        needFork = (arr->owner != ctx->currentArrayOwner);
    } else
#endif
    {
        needFork = (arr->refCount > 1 && arr->owner != (void*) slot);
    }
    if (needFork) {
        GMLArray* clone = GMLArray_clone(arr, intendedOwner);
        GMLArray_decRef(arr);
        slot->array = clone;
        slot->ownsReference = true;
        arr = clone;
    } else if (arr->owner == nullptr) {
        // Claim ownership on first write to an unowned array (e.g. freshly allocated by a builtin).
        arr->owner = intendedOwner;
    }

    // Case 3: grow if needed, then write.
    GMLArray_growTo(arr, index + 1);
    storeIntoArraySlot(GMLArray_slot(arr, index), val);
    return arr;
}

// Public entry point for builtins that materialise an array and return it (layer_get_all).
// Returned RValue holds one strong ref, caller is expected to consume it (stack push / variable write).
// Owner is left null, the first write through a variable slot will claim it.
RValue VM_createArray(MAYBE_UNUSED VMContext* ctx) {
    GMLArray* arr = GMLArray_create(0);
    return RValue_makeArray(arr);
}

// Public helper for builtins that populate an array being returned. Copies val, caller retains ownership.
// The arrayRef must be an RVALUE_ARRAY (as returned by VM_createArray). No CoW fork, the returning array has refCount=1 and no scope owner yet, so we write in place.
void VM_arraySet(MAYBE_UNUSED VMContext* ctx, RValue* arrayRef, int32_t index, RValue val) {
    require(arrayRef != nullptr && arrayRef->type == RVALUE_ARRAY && arrayRef->array != nullptr);
    GMLArray* arr = arrayRef->array;
    GMLArray_growTo(arr, index + 1);
    storeIntoArraySlot(GMLArray_slot(arr, index), val);
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
static bool shouldTraceVariable(StringBooleanEntry* traceMap, const char* scopeName, const char* altScopeName, const char* varName) {
    if (shlen(traceMap) == 0) return false;
    if (shgeti(traceMap, "*") != -1) return true;
    if (shgeti(traceMap, scopeName) != -1) return true;
    if (altScopeName != nullptr && shgeti(traceMap, altScopeName) != -1) return true;
    char formatted[strlen(scopeName) + 1 + strlen(varName) + 1];
    snprintf(formatted, sizeof(formatted), "%s.%s", scopeName, varName);
    if (shgeti(traceMap, formatted) != -1) return true;
    if (altScopeName != nullptr) {
        char altFormatted[strlen(altScopeName) + 1 + strlen(varName) + 1];
        snprintf(altFormatted, sizeof(altFormatted), "%s.%s", altScopeName, varName);
        if (shgeti(traceMap, altFormatted) != -1) return true;
    }
    return false;
}
#endif

// ===[ Array Access Helpers ]===

typedef struct {
    int32_t arrayIndex; // -1 when not an array access
    int32_t instanceType; // Instance type from stack (for VARTYPE_ARRAY / VARTYPE_STACKTOP)
    bool isArray;
    bool hasInstanceType; // true when instanceType was popped from stack
} ArrayAccess;

static int32_t resolveInstanceStackTop(VMContext* ctx) {
    return stackPopInt32(ctx);
}

static const char* varTypeToString(uint8_t varType) {
    switch (varType) {
        case VARTYPE_ARRAY:    return "ARRAY";
        case VARTYPE_STACKTOP: return "STACKTOP";
        case VARTYPE_NORMAL:   return "NORMAL";
        case VARTYPE_INSTANCE: return "INSTANCE";
        default:               return "UNKNOWN";
    }
}

// Pops array index (and optional stacktop value) from the stack if the varRef
// indicates an array or stacktop access. Returns { .arrayIndex = -1, .isArray = false }
// for plain variable access.
static ArrayAccess popArrayAccess(VMContext* ctx, uint32_t varRef) {
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (varType == VARTYPE_ARRAY) {
        // For array reads, GMS pushes: instanceType then arrayIndex (arrayIndex on top)
        int32_t arrayIndex = stackPopInt32(ctx);
        int32_t instanceType = stackPopInt32(ctx);

        // BC17: if instanceType is -9 (INSTANCE_STACKTOP), the actual instance is the next stack item.
        // This is used for chained access like `command_actor[i].specialsprite[arg]` where the array variable's owning instance is resolved from a computed value on the stack.
        if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
            instanceType = resolveInstanceStackTop(ctx);
        }

        return (ArrayAccess){ .arrayIndex = arrayIndex, .instanceType = instanceType, .isArray = true, .hasInstanceType = true };
    }
    if (varType == VARTYPE_STACKTOP) {
        int32_t instanceType = stackPopInt32(ctx);

        // BC17: PushI.e -9 (INSTANCE_STACKTOP) is pushed before the Pop instruction.
        // When we pop -9, it means "the real instance type is the next item on the stack".
        if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
            instanceType = resolveInstanceStackTop(ctx);
        }
        return (ArrayAccess){ .arrayIndex = -1, .isArray = false, .hasInstanceType = true, .instanceType = instanceType };
    }
    return (ArrayAccess){ .arrayIndex = -1, .isArray = false, .hasInstanceType = false };
}

// ===[ Variable Resolution ]===
static const char* instanceTypeName(int32_t instanceType) {
    switch (instanceType) {
        case INSTANCE_SELF: return "self";
        case INSTANCE_OTHER: return "other";
        case INSTANCE_GLOBAL: return "global";
        case INSTANCE_LOCAL: return "local";
        case INSTANCE_ARG: return "arg";
        default: return "instance";
    }
}

// Returns the object name for an instance, or "<global_scope>" for the global scope dummy instance
static const char* instanceObjectName(VMContext* ctx, Instance* inst) {
    if (0 > inst->objectIndex) return "<global_scope>";
    return ctx->dataWin->objt.objects[inst->objectIndex].name;
}

static Variable* resolveVarDef(VMContext* ctx, uint32_t varRef) {
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    return varDef;
}

// Maps a GML local's varID to its slot position in the current code's localVars[] array.
//
// BC16: varIDs for locals are already sequential slot indices (0, 1, 2, ...), so we return the varID unchanged.
//
// BC17+: a single GML local can surface as several VARI chunk entries that share a varID.
// We key by that shared varID via the precomputed currentCodeLocalsSlotMap so reads/writes via any VARI
// entry agree on the same localVars slot.
static uint32_t resolveLocalSlot(VMContext* ctx, int32_t varID) {
    if (IS_BC16_OR_BELOW(ctx)) {
        return (uint32_t) varID;
    }

    // For BC17, we'll allocate the slot dynamically because the data.win CANNOT be trusted to know how localVars the script has
    uint32_t slot = IntIntHashMap_getOrInsertSequential(ctx->currentCodeLocalsSlotMap, varID);
    // Even though we are dynamically allocating the slots, we are still bound to whatever localVars is allocated to
    // So, if a script goes over the MAX_CODE_LOCALS, it would cause unforeseen consequences...
    requireMessage(MAX_CODE_LOCALS > slot, "resolveLocalSlot: exceeded MAX_CODE_LOCALS while allocating a slot for an array-only local");

    // Grow this frame's localVars window to cover `slot` whether the entry is pre-existing or freshly allocated.
    // Pre-existing entries can still be past ctx->localVarCount if a nested call to the same code extended the slot map while the outer frame was suspended (the outer frame's localVarCount is captured at call entry and doesn't follow later growth).
    if (slot >= ctx->localVarCount) {
        for (uint32_t i = ctx->localVarCount; slot >= i; i++) {
            ctx->localVars[i] = (RValue){ .type = RVALUE_UNDEFINED };
        }
        ctx->localVarCount = slot + 1;
    }
    return slot;
}

// Finds an instance by target value.
// target >= 100000: instance ID (find specific instance, including recently-destroyed-but-not-cleaned-up-yet ones so GML code can read properties of an instance just after instance_destroy within the same step).
// target >= 0 && target < 100000: object index (find first ACTIVE instance of that object, checking parent chains)
static Instance* findInstanceByTarget(VMContext* ctx, int32_t target) {
    Runner* runner = (Runner*) ctx->runner;

    if (target >= 100000) {
        // Instance ID - find specific instance
        return hmget(runner->instancesToId, target);
    }

    // Object index - find first active matching instance via the descendant-inclusive bucket. Pure read, no user code, so we walk the bucket directly without an arena snapshot.
    if (target >= 0 && runner->dataWin->objt.count > (uint32_t) target) {
        Instance** bucket = runner->instancesByObject[target];
        int32_t bucketCount = (int32_t) arrlen(bucket);
        for (int32_t i = 0; bucketCount > i; i++) {
            if (bucket[i]->active) return bucket[i];
        }
    }
    return nullptr;
}

// Inline read of a non-array, non-builtin variable from a simple scope.
// Returns false when the instanceType isn't covered or the scope's instance pointer is unavailable, so the caller can fall through to the full resolveVariableRead.
// Used by the OP_PUSH/PUSHLOC/PUSHGLB fast paths in executeLoop to skip the entire resolveVariableRead dispatch overhead.
static inline bool tryFastVarRead(VMContext* ctx, int32_t instanceType, Variable* varDef, RValue* out) {
    switch (instanceType) {
        case INSTANCE_SELF: {
            Instance* inst = (Instance*) ctx->currentInstance;
            if (inst == nullptr) return false;
            RValue* slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            *out = (slot != nullptr) ? *slot : (RValue){ .type = RVALUE_UNDEFINED };
            out->ownsReference = false;
            return true;
        }
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            *out = ctx->localVars[localSlot];
            out->ownsReference = false;
            return true;
        }
        case INSTANCE_GLOBAL: {
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            *out = ctx->globalVars[varDef->varID];
            out->ownsReference = false;
            return true;
        }
        case INSTANCE_OTHER: {
            Instance* inst = (Instance*) ctx->otherInstance;
            if (inst == nullptr) return false;
            RValue* slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            *out = (slot != nullptr) ? *slot : (RValue){ .type = RVALUE_UNDEFINED };
            out->ownsReference = false;
            return true;
        }
    }
    return false;
}

static RValue resolveVariableRead(VMContext* ctx, int32_t instanceType, uint32_t varRef) {
    Variable* varDef = resolveVarDef(ctx, varRef);
    ArrayAccess access = popArrayAccess(ctx, varRef);

    // Use instance type from stack when available (VARTYPE_ARRAY / VARTYPE_STACKTOP)
    int32_t originalInstanceType = instanceType;
    if (access.hasInstanceType) {
        instanceType = access.instanceType;
    }

    // BC17+: Push.v/Pop.v with instrInstanceType == -9 (STACKTOP) and VARTYPE_NORMAL means
    // "the instance is on the stack" (e.g. `struct.field` after @@NewGMLObject@@). Pop it here.
#if IS_BC17_OR_HIGHER_ENABLED
    if (IS_BC17_OR_HIGHER(ctx) && !access.hasInstanceType && instanceType == INSTANCE_STACKTOP) {
        instanceType = resolveInstanceStackTop(ctx);
    }
#endif

    // Resolve target instance for object/instance references (instanceType >= 0)
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
            if (instanceType < 100000 && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                fprintf(stderr, "VM: [%s] READ var '%s' on object index %d (%s) but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            } else {
                fprintf(stderr, "VM: [%s] READ var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            }
            return RValue_makeReal(0.0);
        }
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
    } else if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_ARG) {
        // BC17: argument0..argument15 via INSTANCE_ARG instance type (builtinVarId pre-resolved at parse time)
        int16_t bid = varDef->builtinVarId;
        RValue result;
        if (bid == BUILTIN_VAR_ARGUMENT_COUNT) {
            result = RValue_makeReal((GMLReal) ctx->scriptArgCount);
        } else if (bid == BUILTIN_VAR_ARGUMENT) {
            // argument[N] array-style access
            int32_t idx = access.arrayIndex;
            if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > idx && idx >= 0) {
                result = ctx->scriptArgs[idx];
                result.ownsReference = false;
            } else {
                result = RValue_makeUndefined();
            }
        } else if (bid >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= bid) {
            int32_t argIndex = bid - BUILTIN_VAR_ARGUMENT0;
            if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argIndex) {
                result = ctx->scriptArgs[argIndex];
                result.ownsReference = false;
                // If we are trying to access the argument via an array (example: argName[i]), we NEED to read INSIDE the array
                // Example:
                // function init(arg2) {
                //     var test = arg2[0]; // We NEED to read the [0] from the array
                // }
                // Without this, the caller gets the whole array back
                if (access.isArray && result.type == RVALUE_ARRAY && result.array != nullptr) {
                    result = VM_arrayReadAt(&result, access.arrayIndex);
                }
            } else {
                result = RValue_makeUndefined();
            }
        } else {
            fprintf(stderr, "VM: [%s] INSTANCE_ARG read on unknown variable '%s' (builtinVarId=%d)\n", ctx->currentCodeName, varDef->name, bid);
            result = RValue_makeUndefined();
        }
        return result;
    }

#if IS_BC17_OR_HIGHER_ENABLED
    // BC17+: instanceType == INSTANCE_BUILTIN (-6) on a Push.v means "look up this name as a function reference" (emitted for CallV dispatch paths like `@@This@@(); texture_set_interpolation_ext; CallV`).
    // Intercept before the builtin-variable path: only treat it as a function if the VARI entry isn't a real built-in variable (varID == -6 with a resolved builtinVarId).
    if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_BUILTIN && !(varDef->varID == -6 && varDef->builtinVarId != -1)) {
        // `@@This@@(); push.v bltn.<name>; CallV` is also used for `self.method()` where `method` is a user-defined method stored on the instance (e.g. `init = method(...)` on an object).
        // CallV pops [func, instance, args], so the instance is sitting right below the func we're about to push. Peek at it and try to read `<name>` off its selfVars first; if the VARI entry has a self scope and the peeked slot resolves to an instance with the field, return that method. Otherwise fall through to global function lookup.
        if (varDef->instanceType == INSTANCE_SELF && ctx->stack.top > 0) {
            RValue* peek = stackPeek(ctx);
            int32_t peekId = RValue_toInt32(*peek);
            Instance* peekInst = findInstanceByTarget(ctx, peekId);
            if (peekInst != nullptr) {
                RValue* peekSlot = IntRValueHashMap_findSlot(&peekInst->selfVars, varDef->varID);
                if (peekSlot != nullptr) {
                    RValue val = *peekSlot;
                    val.ownsReference = false;
                    return val;
                }
            }
        }

        // Then try user scripts/code entries (funcMap maps both "funcName" and "gml_Script_funcName")
        ptrdiff_t mapIdx = shgeti(ctx->funcMap, varDef->name);
        if (mapIdx >= 0) {
            int32_t codeIndex = ctx->funcMap[mapIdx].value;
            return RValue_makeMethod(codeIndex, -1);
        }
        // Then try registered built-ins
        ptrdiff_t bidx = shgeti(ctx->builtinMap, (char*) varDef->name);
        if (bidx >= 0) {
            BuiltinFunc bf = ctx->builtinMap[bidx].value;
            return (RValue){ .method = GMLMethod_createBuiltin(bf, -1), .type = RVALUE_METHOD, .ownsReference = true, .gmlStackType = GML_TYPE_VARIABLE };
        }
        // Unresolved: return a method stub so CallV can log a single "unknown function" and return undefined instead of bailing out with a scary "unresolvable function reference" error.
        return (RValue){ .method = GMLMethod_createUnresolved(varDef->name, -1), .type = RVALUE_METHOD, .ownsReference = true, .gmlStackType = GML_TYPE_VARIABLE };
    }
#endif

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        // For object/instance references, temporarily swap currentInstance so VMBuiltins reads the correct instance
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        bool needsInstanceSwap = (instanceType >= 0) || (instanceType == INSTANCE_OTHER);
        if (needsInstanceSwap) ctx->currentInstance = targetInstance;
        RValue result = VMBuiltins_getVariable(ctx, varDef->builtinVarId, varDef->name, access.arrayIndex);
        if (needsInstanceSwap) ctx->currentInstance = savedInstance;

#ifdef ENABLE_VM_TRACING
        // Trace built-in variable reads
        if (instanceType == INSTANCE_GLOBAL) {
            if (shouldTraceVariable(ctx->varReadsToBeTraced, "global", nullptr, varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(result);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] READ global.%s[%d] -> %s (builtin)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                } else {
                    fprintf(stderr, "VM: [%s] READ global.%s -> %s (builtin)\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                }
                free(rvalueAsString);
            }
        } else if (targetInstance != nullptr && targetInstance->objectIndex >= 0 && ctx->dataWin->objt.count > (uint32_t) targetInstance->objectIndex) {
            const char* objName = ctx->dataWin->objt.objects[targetInstance->objectIndex].name;
            if (shouldTraceVariable(ctx->varReadsToBeTraced, objName, "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(result);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, access.arrayIndex, rvalueAsString, targetInstance->instanceId);
                } else {
                    fprintf(stderr, "VM: [%s] READ %s.%s -> %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, rvalueAsString, targetInstance->instanceId);
                }
                free(rvalueAsString);
            }
        }
#endif

        return result;
    }

    // Resolve the variable's scalar slot pointer for the target scope. Array-valued vars live inline as RVALUE_ARRAY in the same slot.
    // VM_arrayReadAt handles the array indirection when access.isArray, VM_arrayWriteAt handles CoW forking when writing.
    RValue* slot = nullptr;
    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            slot = &ctx->localVars[localSlot];
            break;
        }
        case INSTANCE_GLOBAL:
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            slot = &ctx->globalVars[varDef->varID];
            break;
        case INSTANCE_SELF:
        default: {
            Instance* inst = targetInstance;
            if (inst == nullptr) {
                const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
                fprintf(stderr, "VM: [%s] Read on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
                return RValue_makeReal(0.0);
            }
            slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            // sparse storage: nonexistent entry -> treat as undefined scalar (array reads fall through to VM_arrayReadAt returning undefined)
            if (slot == nullptr) {
                if (access.isArray) return (RValue){ .type = RVALUE_UNDEFINED };
                return (RValue){ .type = RVALUE_UNDEFINED };
            }
            break;
        }
    }

    // Array access: read array[index] from the slot.
    if (access.isArray) {
        RValue result = VM_arrayReadAt(slot, access.arrayIndex);
#ifdef ENABLE_VM_TRACING
        const char* scopeName =
            instanceType == INSTANCE_LOCAL ? "local" :
            instanceType == INSTANCE_GLOBAL ? "global" :
            (targetInstance != nullptr ? instanceObjectName(ctx, targetInstance) : "self");
        const char* altName = (instanceType == INSTANCE_SELF || instanceType >= 0 || instanceType == INSTANCE_OTHER) ? "self" : nullptr;
        if (shouldTraceVariable(ctx->varReadsToBeTraced, scopeName, altName, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(result);
            fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s\n", ctx->currentCodeName, scopeName, varDef->name, access.arrayIndex, rvalueAsString);
            free(rvalueAsString);
        }
#endif
        return result;
    }

    // Scalar access: return the slot's current value as a weak view (slot retains ownership).
    RValue result = *slot;
    result.ownsReference = false;

#ifdef ENABLE_VM_TRACING
    // Read tracing for scalar variables
    if (instanceType == INSTANCE_GLOBAL) {
        if (shouldTraceVariable(ctx->varReadsToBeTraced, "global", nullptr, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(result);
            fprintf(stderr, "VM: [%s] READ global.%s -> %s\n", ctx->currentCodeName, varDef->name, rvalueAsString);
            free(rvalueAsString);
        }
    } else if (instanceType == INSTANCE_SELF || instanceType >= 0) {
        Instance* inst = targetInstance;
        if (inst != nullptr && shouldTraceVariable(ctx->varReadsToBeTraced, instanceObjectName(ctx, inst), "self", varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(result);
            fprintf(stderr, "VM: [%s] READ %s.%s -> %s (instanceId=%d)\n", ctx->currentCodeName, instanceObjectName(ctx, inst), varDef->name, rvalueAsString, inst->instanceId);
            free(rvalueAsString);
        }
    }
#endif

    return result;
}

// Helper: write a variable value to a single specific instance (always copies, never moves the original val)
static void writeSingleInstanceVariable(VMContext* ctx, Instance* inst, Variable* varDef, ArrayAccess* access, RValue val) {
    // Built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        ctx->currentInstance = inst;
        VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, access->arrayIndex);
        ctx->currentInstance = savedInstance;
        return;
    }

    // Array write - materialise-on-write via VM_arrayWriteAt. getOrInsertUndefined returns the existing slot or inserts an UNDEFINED entry and returns it.
    if (access->isArray) {
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
        VM_arrayWriteAt((VMContext*) ctx, slot, access->arrayIndex, val);
        return;
    }

    // Scalar write (Instance_setSelfVar always takes an independent ref; caller still owns "val").
    Instance_setSelfVar(inst, varDef->varID, val);
}

// Transfer ownership of "val into "*dest", freeing the old value first.
// Strings are duplicated only if the source view is non-owning (so we don't double-free).
// Arrays/methods/structs bump refcount when needed and flip the source's ownsReference flag to take a strong ref.
static inline void writeIntoSlot(RValue* dest, RValue val) {
    RValue_free(dest);
    if (val.type == RVALUE_STRING && !val.ownsReference && val.string != nullptr) {
        *dest = RValue_makeOwnedString(safeStrdup(val.string));
    } else if (val.type == RVALUE_ARRAY && val.array != nullptr) {
        if (!val.ownsReference) GMLArray_incRef(val.array);
        val.ownsReference = true;
        *dest = val;
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (val.type == RVALUE_METHOD && val.method != nullptr) {
        if (!val.ownsReference) GMLMethod_incRef(val.method);
        val.ownsReference = true;
        *dest = val;
#endif
    } else if (val.type == RVALUE_STRUCT && val.structInst != nullptr) {
        if (!val.ownsReference) Instance_structIncRef(val.structInst);
        val.ownsReference = true;
        *dest = val;
    } else {
        *dest = val;
    }
}

// Force out-of-line so the OP_POP fast path in executeLoop doesn't inline this, because we already have an "optimized" version for common writes
__attribute__((noinline))
static void resolveVariableWrite(VMContext* ctx, int32_t instanceType, uint32_t varRef, RValue val) {
    Variable* varDef = resolveVarDef(ctx, varRef);

    // Fast path: When the varType==VARTYPE_NORMAL...
    // * We can skip the popArrayAccess
    // * We can skip the BC17 STACKTOP and INSTANCE_ARG branches
    // * We can skip the array-write block itself
    // * We can skip BOTH instanceType switches
    if (varDef->varID >= 0) {
        switch (instanceType) {
            case INSTANCE_LOCAL: {
                uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                require(ctx->localVarCount > localSlot);
                writeIntoSlot(&ctx->localVars[localSlot], val);
                return;
            }
            case INSTANCE_GLOBAL: {
                require(ctx->globalVarCount > (uint32_t) varDef->varID);
                writeIntoSlot(&ctx->globalVars[varDef->varID], val);
                return;
            }
            case INSTANCE_SELF: {
                Instance* inst = (Instance*) ctx->currentInstance;
                if (inst != nullptr) {
                    Instance_setSelfVar(inst, varDef->varID, val);
                    RValue_free(&val);
                    return;
                }
                break; // fall through to slow path so the existing nullptr-instance error gets logged
            }
            case INSTANCE_OTHER: {
                Instance* inst = (Instance*) ctx->otherInstance;
                if (inst != nullptr) {
                    Instance_setSelfVar(inst, varDef->varID, val);
                    RValue_free(&val);
                    return;
                }
                break; // fall through (otherInstance was nullptr, slow path will use currentInstance)
            }
        }
    }

    // The slow path is used for builtin vars, object/instance references (instanceType >= 0), INSTANCE_ARG/STACKTOP, and other miscellaneous things like if we get a nullptr above
    ArrayAccess access = popArrayAccess(ctx, varRef);

    // Use instance type from stack when available (VARTYPE_ARRAY / VARTYPE_STACKTOP)
    int32_t originalInstanceType = instanceType;
    if (access.hasInstanceType) {
        instanceType = access.instanceType;
    }

    // BC17+: Pop.v with instrInstanceType == -9 (STACKTOP) and VARTYPE_NORMAL means
    // "the instance is on the stack" (e.g. `struct.field =` after @@NewGMLObject@@). Pop it here.
#if IS_BC17_OR_HIGHER_ENABLED
    if (IS_BC17_OR_HIGHER(ctx) && !access.hasInstanceType && instanceType == INSTANCE_STACKTOP) {
        instanceType = resolveInstanceStackTop(ctx);
    }
#endif

    // GML: writing through an object reference (obj_foo.var = val) sets the variable on ALL instances of that object. The setter (writeSingleInstanceVariable) can run user code, so iterate a snapshot of the bucket.
    if (instanceType >= 0 && 100000 > instanceType) {
        Runner* runner = (Runner*) ctx->runner;
        bool found = false;
        int32_t snapBase = Runner_pushInstancesOfObject(runner, instanceType);
        int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
        for (int32_t i = snapBase; snapEnd > i; i++) {
            Instance* inst = runner->instanceSnapshots[i];
            if (!inst->active) continue;
            found = true;
            writeSingleInstanceVariable(ctx, inst, varDef, &access, val);
#ifdef ENABLE_VM_TRACING
            if (shouldTraceVariable(ctx->varWritesToBeTraced, instanceObjectName(ctx, inst), "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d, all-instances object write)\n", ctx->currentCodeName, instanceObjectName(ctx, inst), varDef->name, rvalueAsString, inst->instanceId);
                free(rvalueAsString);
            }
#endif
        }
        Runner_popInstanceSnapshot(runner, snapBase);
        if (!found) {
            if (ctx->dataWin->objt.count > (uint32_t) instanceType) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                char* valAsString = RValue_toString(val);
                fprintf(stderr, "VM: [%s] WRITE var '%s' on object %d (%s) but no instances found (value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, valAsString);
                free(valAsString);
            }
        }
        RValue_free(&val);
        return;
    }

    // Resolve target instance for instance ID references (instanceType >= 100000) or special types
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
            char* valAsString = RValue_toString(val);
            fprintf(stderr, "VM: [%s] WRITE var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
            free(valAsString);
            return;
        }
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
    } else if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_ARG) {
        // BC17: write to argument0..argument15 via INSTANCE_ARG instance type (builtinVarId pre-resolved at parse time)
        int16_t bid = varDef->builtinVarId;
        int32_t writeIndex = -1;
        if (bid >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= bid) {
            writeIndex = bid - BUILTIN_VAR_ARGUMENT0;
        } else if (bid == BUILTIN_VAR_ARGUMENT) {
            writeIndex = access.arrayIndex;
        } else {
            fprintf(stderr, "VM: [%s] INSTANCE_ARG write on unknown variable '%s' (builtinVarId=%d)\n", ctx->currentCodeName, varDef->name, bid);
        }
        if (writeIndex >= 0 && GML_MAX_ARGUMENTS > writeIndex && ctx->scriptArgs != nullptr) {
            RValue_free(&ctx->scriptArgs[writeIndex]);
            if (val.type == RVALUE_STRING && val.string != nullptr) {
                ctx->scriptArgs[writeIndex] = RValue_makeOwnedString(safeStrdup(val.string));
            } else {
                // Transfer ownership from val into scriptArgs: copy the tagged union as-is and neutralize val so the RValue_free below is a no-op for arrays/methods.
                ctx->scriptArgs[writeIndex] = val;
                val.ownsReference = false;
            }
            if (writeIndex >= ctx->scriptArgCount) {
                ctx->scriptArgCount = writeIndex + 1;
            }
        }
        RValue_free(&val);
        return;
    }

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        // For object/instance references, temporarily swap currentInstance so VMBuiltins writes the correct instance
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        bool needsInstanceSwap = (instanceType >= 0) || (instanceType == INSTANCE_OTHER);
        if (needsInstanceSwap) ctx->currentInstance = targetInstance;
        VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, access.arrayIndex);
        if (needsInstanceSwap) ctx->currentInstance = savedInstance;

#ifdef ENABLE_VM_TRACING
        // Trace built-in variable writes
        if (instanceType == INSTANCE_GLOBAL) {
            if (shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s (builtin)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                } else {
                    fprintf(stderr, "VM: [%s] WRITE global.%s = %s (builtin)\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                }
                free(rvalueAsString);
            }
        } else if (targetInstance != nullptr && targetInstance->objectIndex >= 0 && ctx->dataWin->objt.count > (uint32_t) targetInstance->objectIndex) {
            const char* objName = ctx->dataWin->objt.objects[targetInstance->objectIndex].name;
            if (shouldTraceVariable(ctx->varWritesToBeTraced, objName, "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, access.arrayIndex, rvalueAsString, targetInstance->instanceId);
                } else {
                    fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, rvalueAsString, targetInstance->instanceId);
                }
                free(rvalueAsString);
            }
        }
#endif

        // VMBuiltins_setVariable reads values (toReal, toInt32, etc.) but does not take ownership
        RValue_free(&val);
        return;
    }

    // Resolve the slot pointer for this scope. For INSTANCE_SELF we materialise a sparse selfVars entry if it doesn't exist so VM_arrayWriteAt has a stable slot to own.
    RValue* slot = nullptr;
    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            slot = &ctx->localVars[localSlot];
            break;
        }
        case INSTANCE_GLOBAL:
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            slot = &ctx->globalVars[varDef->varID];
            break;
        case INSTANCE_SELF:
        default: {
            Instance* inst = targetInstance;
            if (inst == nullptr) {
                const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
                char* valAsString = RValue_toString(val);
                fprintf(stderr, "VM: [%s] Write on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
                free(valAsString);
                RValue_free(&val);
                return;
            }
            slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
            break;
        }
    }

    // Array write via VM_arrayWriteAt (handles CoW fork, grow, owner stamping).
    if (access.isArray) {
        VM_arrayWriteAt(ctx, slot, access.arrayIndex, val);
#ifdef ENABLE_VM_TRACING
        const char* scopeName =
            instanceType == INSTANCE_LOCAL ? "local" :
            instanceType == INSTANCE_GLOBAL ? "global" :
            (targetInstance != nullptr ? instanceObjectName(ctx, targetInstance) : "self");
        const char* altName = (instanceType == INSTANCE_SELF || instanceType >= 0 || instanceType == INSTANCE_OTHER) ? "self" : nullptr;
        if (shouldTraceVariable(ctx->varWritesToBeTraced, scopeName, altName, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(val);
            fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s\n", ctx->currentCodeName, scopeName, varDef->name, access.arrayIndex, rvalueAsString);
            free(rvalueAsString);
        }
#endif
        RValue_free(&val);
        return;
    }

#ifdef ENABLE_VM_TRACING
    bool shouldLogGlobal = false;
    bool shouldLogInstance = false;
#endif

    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            writeIntoSlot(&ctx->localVars[localSlot], val);
            return;
        }
        case INSTANCE_GLOBAL: {
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            RValue* dest = &ctx->globalVars[varDef->varID];
            writeIntoSlot(dest, val);
#ifdef ENABLE_VM_TRACING
            if (shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(*dest);
                fprintf(stderr, "VM: [%s] WRITE global.%s = %s\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                free(rvalueAsString);
            }
#endif
            return;
        }
        case INSTANCE_SELF:
        default: {
            // Self or object/instance reference - use sparse hashmap
            Instance* inst = targetInstance;
            Instance_setSelfVar(inst, varDef->varID, val);
#ifdef ENABLE_VM_TRACING
            if (shouldTraceVariable(ctx->varWritesToBeTraced, instanceObjectName(ctx, inst), "self", varDef->name)) {
                RValue written = Instance_getSelfVar(inst, varDef->varID);
                char* rvalueAsString = RValue_toStringTyped(written);
                fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d)\n", ctx->currentCodeName, instanceObjectName(ctx, inst), varDef->name, rvalueAsString, inst->instanceId);
                free(rvalueAsString);
            }
#endif
            // Instance_setSelfVar always copies strings, so free the original
            RValue_free(&val);
            return;
        }
    }
}

// ===[ Type Conversion ]===

static RValue convertValue(RValue val, uint8_t targetType) {
    switch (targetType) {
        case GML_TYPE_DOUBLE:
            return RValue_makeReal(RValue_toReal(val));
        case GML_TYPE_FLOAT:
            return RValue_makeReal((GMLReal) (float) RValue_toReal(val));
        case GML_TYPE_INT32:
            return RValue_makeInt32(RValue_toInt32(val));
        case GML_TYPE_INT64:
            return RValue_makeInt64(RValue_toInt64(val));
        case GML_TYPE_BOOL:
            return RValue_makeBool(RValue_toBool(val));
        case GML_TYPE_STRING: {
            char* str = RValue_toString(val);
            return RValue_makeOwnedString(str);
        }
        case GML_TYPE_VARIABLE:
            // Variable type on stack is just an RValue passthrough
            return val;
        default:
            fprintf(stderr, "VM: Unknown target type 0x%X for conversion\n", targetType);
            return val;
    }
}

// ===[ Opcode Handlers ]===

static void handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint8_t type1) {
    switch (type1) {
        case GML_TYPE_DOUBLE:
            stackPush(ctx, RValue_makeReal(BinaryUtils_readFloat64Aligned(extraData)));
            break;
        case GML_TYPE_FLOAT:
            stackPush(ctx, RValue_makeReal((GMLReal) BinaryUtils_readFloat32Aligned(extraData)));
            break;
        case GML_TYPE_INT32:
            stackPush(ctx, RValue_makeInt32(BinaryUtils_readInt32Aligned(extraData)));
            break;
        case GML_TYPE_INT64:
            stackPush(ctx, RValue_makeInt64(BinaryUtils_readInt64Aligned(extraData)));
            break;
        case GML_TYPE_BOOL:
            stackPush(ctx, RValue_makeBool(BinaryUtils_readInt32Aligned(extraData) != 0));
            break;
        case GML_TYPE_VARIABLE: {
            int32_t instanceType = (int32_t) instrInstanceType(instr);
            uint32_t varRef = resolveVarOperand(extraData);
            uint8_t varType = (varRef >> 24) & 0xF8;
#if IS_BC17_OR_HIGHER_ENABLED
            if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
                // V17: multi-dim first-step. Stack has [scope, firstIndex] (with an optional real-instance slot underneath when scope == -9 INSTANCE_STACKTOP).
                // We resolve the variable's top-level array slot, materialise it if needed, then drill into arr->data[firstIndex] (materialising a sub-array there too).
                // The sub-array is pushed as a weak ref; subsequent BREAK_PUSHAC/PUSHAF/POPAF consume it.
                Variable* varDef = resolveVarDef(ctx, varRef);
                int32_t firstIndex = stackPopInt32(ctx);
                int32_t scope = stackPopInt32(ctx);
                if (IS_BC17_OR_HIGHER(ctx) && scope == INSTANCE_STACKTOP) {
                    scope = resolveInstanceStackTop(ctx);
                }

                // Resolve the slot for this scope.
                RValue* slot = nullptr;
                switch (scope) {
                    case INSTANCE_LOCAL: {
                        uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                        require(ctx->localVarCount > localSlot);
                        slot = &ctx->localVars[localSlot];
                        break;
                    }
                    case INSTANCE_GLOBAL:
                        require(ctx->globalVarCount > (uint32_t) varDef->varID);
                        slot = &ctx->globalVars[varDef->varID];
                        break;
                    case INSTANCE_SELF:
                    case INSTANCE_OTHER: {
                        Instance* inst = (scope == INSTANCE_OTHER && ctx->otherInstance != nullptr)
                            ? (Instance*) ctx->otherInstance
                            : (Instance*) ctx->currentInstance;
                        require(inst != nullptr);
                        slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
                        break;
                    }
                    default: {
                        Instance* inst = findInstanceByTarget(ctx, scope);
                        if (inst == nullptr) {
                            fprintf(stderr, "VM: ARRAYPUSHAF: no instance for scope %d varID=%d\n", scope, varDef->varID);
                            abort();
                        }
                        slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
                        break;
                    }
                }

                // Materialise the top-level array in the slot if needed.
                if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
                    RValue_free(slot);
                    GMLArray* fresh = GMLArray_create(0);
                    fresh->owner = IS_BC17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
                    *slot = RValue_makeArray(fresh);
                }
                GMLArray* top = slot->array;
                GMLArray_growTo(top, firstIndex + 1);
                RValue* topSlot = GMLArray_slot(top, firstIndex);
                // Materialise the sub-array at [firstIndex] if it's not already an array.
                if (topSlot->type != RVALUE_ARRAY || topSlot->array == nullptr) {
                    RValue_free(topSlot);
                    GMLArray* sub = GMLArray_create(0);
                    sub->owner = top->owner;
                    *topSlot = (RValue){ .array = sub, .type = RVALUE_ARRAY, .ownsReference = true, RVALUE_INIT_GMLTYPE(GML_TYPE_VARIABLE) };
                }
                // Push a weak ref to the sub-array — short-lived, consumed by the next BREAK op.
                stackPush(ctx, RValue_makeArrayWeak(topSlot->array));
            } else
#endif
            {
                RValue val = resolveVariableRead(ctx, instanceType, varRef);
                // Mark as variable-width (16 bytes on native stack) regardless of the RValue's actual type
                stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
            }
            break;
        }
        case GML_TYPE_STRING: {
            int32_t stringIndex = BinaryUtils_readInt32Aligned(extraData);
            require(stringIndex >= 0 && ctx->dataWin->strg.count > (uint32_t) stringIndex);
            stackPush(ctx, RValue_makeString(ctx->dataWin->strg.strings[stringIndex]));
            break;
        }
        case GML_TYPE_INT16: {
            int16_t value = (int16_t) (instr & 0xFFFF);
            RValue val = RValue_makeInt32((int32_t) value);
            stackPushTyped(ctx, val, GML_TYPE_INT16);
            break;
        }
        default:
            fprintf(stderr, "VM: Push with unknown type 0x%X\n", type1);
            abort();
    }
}

#if IS_BC17_OR_HIGHER_ENABLED
// For V17+ VARTYPE_ARRAYPUSHAF/POPAF on a top-level variable: return the slot's GMLArray*,
// materialising a fresh empty one in the slot if it isn't an array yet. Used by PushLoc/Glb/Bltn.
// Pushes a weak ref onto the stack — short-lived, consumed by the next BREAK_PUSHAC/PUSHAF/POPAF.
static void pushTopLevelArrayRef(VMContext* ctx, RValue* slot) {
    if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
        RValue_free(slot);
        GMLArray* fresh = GMLArray_create(0);
        fresh->owner = IS_BC17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
        *slot = RValue_makeArray(fresh);
    }
    stackPush(ctx, RValue_makeArrayWeak(slot->array));
}
#endif

static void handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    uint32_t varRef = resolveVarOperand(extraData);
#if IS_BC17_OR_HIGHER_ENABLED
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
        Variable* varDef = resolveVarDef(ctx, varRef);
        int32_t scope = (int32_t) instrInstanceType(instr);
        Instance* inst = nullptr;
        if (scope == INSTANCE_SELF || scope == -1) {
            inst = (Instance*) ctx->currentInstance;
        } else if (scope == INSTANCE_OTHER && ctx->otherInstance != nullptr) {
            inst = (Instance*) ctx->otherInstance;
        } else if (scope >= 0) {
            inst = findInstanceByTarget(ctx, scope);
        } else {
            inst = (Instance*) ctx->currentInstance;
        }
        if (inst == nullptr) {
            fprintf(stderr, "VM: PushBltn ARRAYPUSHAF: no instance for scope %d varID=%d\n", scope, varDef->varID);
            abort();
        }
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
        pushTopLevelArrayRef(ctx, slot);
        return;
    }
#endif
    RValue val = resolveVariableRead(ctx, (int32_t) instrInstanceType(instr), varRef);
    stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
}

static void handlePushI(VMContext* ctx, uint32_t instr) {
    int16_t value = (int16_t) (instr & 0xFFFF);
    RValue val = RValue_makeInt32((int32_t) value);
    stackPushTyped(ctx, val, GML_TYPE_INT16);
}

static void handlePop(VMContext* ctx, uint32_t instr, uint8_t type1, uint32_t varRef, uint8_t varType, int32_t instanceType) {
    uint8_t type2 = instrType2(instr);   // source type (what's on stack)

    RValue val;
    int32_t arrayIndex = -1;

    int32_t originalInstanceType = instanceType;
    if (varType == VARTYPE_ARRAY) {
        if (type1 == GML_TYPE_VARIABLE) {
            // Simple assignment (Pop.v.v): stack bottom-to-top = [value, (realInstance,) instanceType, arrayIndex]
            arrayIndex = stackPopInt32(ctx);
            instanceType = stackPopInt32(ctx);

            // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance ID/object index" (e.g. `su_actor.specialsprite[0] = ...`)
            if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
                instanceType = resolveInstanceStackTop(ctx);
            }

            val = stackPop(ctx);
        } else {
            // Compound assignment (Pop.i.v, etc.): stack bottom-to-top = [(realInstance,) instanceType, arrayIndex, value]
            val = stackPop(ctx);

            arrayIndex = stackPopInt32(ctx);
            instanceType = stackPopInt32(ctx);

            // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance ID/object index"
            if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
                instanceType = resolveInstanceStackTop(ctx);
            }
        }
    } else if (varType == VARTYPE_STACKTOP && type1 == GML_TYPE_VARIABLE) {
        // Simple assignment (Pop.v.v) with STACKTOP: stack bottom-to-top = [value, instanceType]
        // Pop instanceType first (top), then value (bottom)
        instanceType = stackPopInt32(ctx);

        // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance type"
        if (IS_BC17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
            instanceType = resolveInstanceStackTop(ctx);
        }

        val = stackPop(ctx);

        // Clear STACKTOP type bits so resolveVariableWrite's popArrayAccess won't double-pop
        varRef = (varRef & 0x07FFFFFF) | ((uint32_t) VARTYPE_NORMAL << 24);
    } else {
        val = stackPop(ctx);
    }

    // Convert if source type differs from destination type.
    // For VARTYPE_ARRAY compound assignments (type1 != GML_TYPE_VARIABLE), the type1 field
    // indicates the stack layout (compound vs simple), NOT a type conversion target.
    // Skip conversion in that case to preserve string values through += operations.
    // For compound assignments (type1 != GML_TYPE_VARIABLE) with VARTYPE_ARRAY or VARTYPE_STACKTOP,
    // the type1 field indicates the stack layout (compound vs simple), NOT a type conversion target.
    // Skip conversion to preserve the actual computed value (e.g. g.image_angle -= 4.5 must not truncate to int).
    bool isCompoundAssignment = ((varType == VARTYPE_ARRAY || varType == VARTYPE_STACKTOP) && type1 != GML_TYPE_VARIABLE);
    if (type2 != type1 && type1 != GML_TYPE_VARIABLE && !isCompoundAssignment) {
        RValue converted = convertValue(val, type1);
        RValue_free(&val);
        val = converted;
    }

    // When storing into a variant variable from an int32/int64 stack source, coerce to real.
    // GMS variables normalize integer literals to doubles so subsequent arithmetic routes through the real fast path instead of int32 x int32 wrapping.
    if (type1 == GML_TYPE_VARIABLE && !isCompoundAssignment && (type2 == GML_TYPE_INT32 || type2 == GML_TYPE_INT64 || type2 == GML_TYPE_INT16)) {
        if (val.type == RVALUE_INT32) {
            val = RValue_makeReal((GMLReal) val.int32);
        }
#ifndef NO_RVALUE_INT64
        else if (val.type == RVALUE_INT64) {
            val = RValue_makeReal((GMLReal) val.int64);
        }
#endif
    }

    if (varType == VARTYPE_ARRAY) {
        Variable* varDef = resolveVarDef(ctx, varRef);
        if (varDef->varID == -6) {
            // Resolve target instance for built-in array variable writes (e.g. obj_foo.alarm[0] = 2)
            if (instanceType >= 0 && 100000 > instanceType) {
                // Object reference: write to ALL instances of that object. The setter can run user code, so iterate a snapshot of the bucket.
                Runner* runner = (Runner*) ctx->runner;
                Instance* savedInstance = (Instance*) ctx->currentInstance;
                int32_t snapBase = Runner_pushInstancesOfObject(runner, instanceType);
                int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
                for (int32_t i = snapBase; snapEnd > i; i++) {
                    Instance* inst = runner->instanceSnapshots[i];
                    if (!inst->active) continue;
                    ctx->currentInstance = inst;
                    VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, arrayIndex);
                }
                Runner_popInstanceSnapshot(runner, snapBase);
                ctx->currentInstance = savedInstance;
            } else if (instanceType >= 0) {
                // Instance ID reference
                Instance* target = findInstanceByTarget(ctx, instanceType);
                if (target != nullptr) {
                    Instance* savedInstance = (Instance*) ctx->currentInstance;
                    ctx->currentInstance = target;
                    VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, arrayIndex);
                    ctx->currentInstance = savedInstance;
                }
            } else if (instanceType == INSTANCE_OTHER && ctx->otherInstance != nullptr) {
                Instance* savedInstance = (Instance*) ctx->currentInstance;
                ctx->currentInstance = (Instance*) ctx->otherInstance;
                VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, arrayIndex);
                ctx->currentInstance = savedInstance;
            } else {
                // INSTANCE_SELF or other special types: use current instance
                VMBuiltins_setVariable(ctx, varDef->builtinVarId, varDef->name, val, arrayIndex);
            }
        } else {
            // Resolve slot for this scope: VM_arrayWriteAt handles CoW + materialisation + grow.
            RValue* slot = nullptr;
            switch (instanceType) {
                case INSTANCE_LOCAL: {
                    uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                    require(ctx->localVarCount > localSlot);
                    slot = &ctx->localVars[localSlot];
                    break;
                }
                case INSTANCE_GLOBAL:
                    require(ctx->globalVarCount > (uint32_t) varDef->varID);
                    slot = &ctx->globalVars[varDef->varID];
                    break;
                case INSTANCE_SELF:
                default: {
                    struct Instance* inst = (struct Instance*) ctx->currentInstance;
                    if (instanceType >= 0) {
                        inst = findInstanceByTarget(ctx, instanceType);
                        if (inst == nullptr) {
                            const char* varTypeName = varTypeToString(varType);
                            char* valAsString = RValue_toString(val);
                            if (instanceType < 100000 && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                                fprintf(stderr, "VM: [%s] WRITE array var '%s[%d]' on object index %d (%s) but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, ctx->dataWin->objt.objects[instanceType].name, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            } else {
                                fprintf(stderr, "VM: [%s] WRITE array var '%s[%d]' on instance %d but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            }
                            free(valAsString);
                            RValue_free(&val);
                            return;
                        }
                    } else if (instanceType == INSTANCE_OTHER && ctx->otherInstance != nullptr) {
                        inst = (Instance*) ctx->otherInstance;
                    }
                    if (inst == nullptr) {
                        RValue_free(&val);
                        return;
                    }
                    slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
                    break;
                }
            }
            if (slot != nullptr) {
                VM_arrayWriteAt(ctx, slot, arrayIndex, val);
#ifdef ENABLE_VM_TRACING
                bool isSelfScope = (instanceType != INSTANCE_LOCAL && instanceType != INSTANCE_GLOBAL);
                const char* scopeName = instanceType == INSTANCE_LOCAL ? "local" : instanceType == INSTANCE_GLOBAL ? "global" : "self";
                if (shouldTraceVariable(ctx->varWritesToBeTraced, scopeName, isSelfScope ? nullptr : "self", varDef->name)) {
                    char* rvalueAsString = RValue_toString(val);
                    fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s\n", ctx->currentCodeName, scopeName, varDef->name, arrayIndex, rvalueAsString);
                    free(rvalueAsString);
                }
#endif
            }
            RValue_free(&val);
        }
    } else {
        resolveVariableWrite(ctx, instanceType, varRef, val);
    }
}

static void handlePopz(VMContext* ctx) {
    RValue val = stackPop(ctx);
    RValue_free(&val);
}

__attribute__((noinline))
static void handleAddString(VMContext* ctx, RValue a, RValue b, uint8_t resultType) {
    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        // String concatenation
        const char* sa = a.string != nullptr ? a.string : "";
        const char* sb = b.string != nullptr ? b.string : "";
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = safeMalloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeOwnedString(result), resultType);
    } else {
        // String + Number: convert both to strings and concatenate (GMS behavior)
        char* sa = RValue_toString(a);
        char* sb = RValue_toString(b);
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = safeMalloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        free(sa);
        free(sb);
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeOwnedString(result), resultType);
    }
}

__attribute__((noinline))
static void handleMulString(VMContext* ctx, RValue a, RValue b, uint8_t resultType) {
    // a.type == RVALUE_STRING; b is the repetition count.
    int count = RValue_toInt32(b);
    const char* str = a.string != nullptr ? a.string : "";
    size_t len = strlen(str);
    if (0 >= count || len == 0) {
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeOwnedString(safeStrdup("")), resultType);
    } else {
        char* result = safeMalloc(len * count + 1);
        repeat(count, i) {
            memcpy(result + i * len, str, len);
        }
        result[len * count] = '\0';
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeOwnedString(result), resultType);
    }
}

static void handleDiv(VMContext* ctx, uint32_t instr) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    GMLReal divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoDiv :: Divide by zero\n");
        abort();
    }
    GMLReal result = RValue_toReal(a) / divisor;
    RValue_free(&a);
    RValue_free(&b);
    stackPushTyped(ctx, RValue_makeReal(result), instrType2(instr));
}

static void handleRem(VMContext* ctx, uint32_t instr) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    int32_t ib = RValue_toInt32(b);
    if (ib == 0) {
        fprintf(stderr, "VM: DoRem :: Divide by zero\n");
        abort();
    }
    int32_t result = RValue_toInt32(a) % ib;
    RValue_free(&a);
    RValue_free(&b);
    stackPushTyped(ctx, RValue_makeInt32(result), instrType2(instr));
}

static void handleMod(VMContext* ctx, uint32_t instr) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    GMLReal divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoMod :: Divide by zero\n");
        abort();
    }
    GMLReal result = GMLReal_fmod(RValue_toReal(a), divisor);
    RValue_free(&a);
    RValue_free(&b);
    stackPushTyped(ctx, RValue_makeReal(result), instrType2(instr));
}

#define SIMPLE_BYTECODE_BITWISE_OPERATION(op) \
    int32_t b = stackPopInt32(ctx); \
    int32_t a = stackPopInt32(ctx); \
    int32_t result = a op b; \
    stackPushTyped(ctx, RValue_makeInt32(result), instrType2(instr))

static void handleAnd(VMContext* ctx, uint32_t instr) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(&);
}

static void handleOr(VMContext* ctx, uint32_t instr) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(|);
}

static void handleXor(VMContext* ctx, uint32_t instr) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(^);
}

static void handleNeg(VMContext* ctx, uint32_t instr) {
    RValue a = stackPop(ctx);
    GMLReal result = -RValue_toReal(a);
    RValue_free(&a);
    stackPushTyped(ctx, RValue_makeReal(result), instrType1(instr));
}

static void handleNot(VMContext* ctx, uint32_t instr) {
    uint8_t resultType = instrType1(instr);
    int32_t a = stackPopInt32(ctx);
    if (GML_TYPE_BOOL == resultType) {
        // Logical NOT: compiler emits this for the ! operator on boolean expressions
        int32_t result = (a == 0) ? 1 : 0;
        stackPushTyped(ctx, RValue_makeBool(result != 0), resultType);
    } else {
        // Bitwise NOT: used for ~ operator on integer types
        int32_t result = ~a;
        stackPushTyped(ctx, RValue_makeInt32(result), resultType);
    }
}

static void handleShl(VMContext* ctx, uint32_t instr) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(<<);
}

static void handleShr(VMContext* ctx, uint32_t instr) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(>>);
}

static void handleConv(VMContext* ctx, uint8_t srcType, uint8_t dstType, uint8_t convKey) {
    RValue val = stackPop(ctx);

    RValue result;

    switch (convKey) {
        // Identity conversions (no-op)
        case 0x00: case 0x22: case 0x33: case 0x44: case 0x66:
            result = val;
            break;

        // Double (0) -> other
        case 0x20: result = RValue_makeInt32((int32_t) val.real); break;
        case 0x30: result = RValue_makeInt64((int64_t) val.real); break;
        case 0x40: result = RValue_makeBool(val.real > 0.5); break;
        case 0x50: result = val; break; // Double -> Variable (passthrough)
        case 0x60: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF0: result = RValue_makeInt32((int32_t) val.real); break;

        // Float (1) -> other (float stored as double in our RValue)
        case 0x01: result = RValue_makeReal(val.real); break;
        case 0x21: result = RValue_makeInt32((int32_t) val.real); break;
        case 0x31: result = RValue_makeInt64((int64_t) val.real); break;
        case 0x41: result = RValue_makeBool(val.real > 0.5); break;
        case 0x51: result = val; break; // Float -> Variable (passthrough)

        // Int32 (2) -> other
        case 0x02: result = RValue_makeReal((GMLReal) val.int32); break;
        case 0x12: result = RValue_makeReal((GMLReal) val.int32); break;
        case 0x32: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x42: result = RValue_makeBool(val.int32 > 0); break;
        case 0x52: result = val; break; // Int32 -> Variable (passthrough)
        case 0x62: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF2: result = val; break;

#ifndef NO_RVALUE_INT64
        // Int64 (3) -> other
        case 0x03: result = RValue_makeReal((GMLReal) val.int64); break;
        case 0x23: result = RValue_makeInt32((int32_t) val.int64); break;
        case 0x43: result = RValue_makeBool(val.int64 > 0); break;
        case 0x53: result = val; break; // Int64 -> Variable (passthrough)
#elif IS_BC17_OR_HIGHER_ENABLED
        // Int64 (3) -> other (Int64 stored as Int32 when NO_RVALUE_INT64).
        // Only emitted on BC17+ builds: BC16 games (Undertale, SURVEY_PROGRAM) never emit Int64 Conv opcodes.
        case 0x03: result = RValue_makeReal((GMLReal) val.int32); break;
        case 0x23: result = val; break; // Already Int32
        case 0x43: result = RValue_makeBool(val.int32 > 0); break;
        case 0x53: result = val; break; // Int64 -> Variable (passthrough)
#endif

        // Bool (4) -> other
        case 0x04: result = RValue_makeReal((GMLReal) val.int32); break;
        case 0x24: result = RValue_makeInt32(val.int32); break;
        case 0x34: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x54: result = val; break; // Bool -> Variable (passthrough)
        case 0x64: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }

        // Variable (5) -> other
        case 0x05: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x15: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x25: result = RValue_makeInt32(RValue_toInt32(val)); break;
        case 0x35: result = RValue_makeInt64(RValue_toInt64(val)); break;
        case 0x45: result = RValue_makeBool(RValue_toBool(val)); break;
        case 0x55: result = val; break; // Variable -> Variable (identity)
        case 0x65: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF5: result = RValue_makeInt32(RValue_toInt32(val)); break;

        // String (6) -> other
        case 0x06: result = RValue_makeReal(GMLReal_strtod(val.string, nullptr)); break;
        case 0x26: result = RValue_makeInt32((int32_t) GMLReal_strtod(val.string, nullptr)); break;
        case 0x36: result = RValue_makeInt64((int64_t) GMLReal_strtod(val.string, nullptr)); break;
        case 0x46: result = RValue_makeBool(val.string != nullptr && val.string[0] != '\0'); break;
        case 0x56: {
            // String -> Variable: keep as-is since our RValue handles strings natively
            result = val;
            break;
        }

        // Int16 (F) -> other
        case 0x0F: result = RValue_makeReal((GMLReal) val.int32); break;
        case 0x2F: result = val; break;
        case 0x5F: result = val; break;

        default:
            fprintf(stderr, "VM: Conv unhandled conversion 0x%02X (src=0x%X dst=0x%X)\n", convKey, srcType, dstType);
            result = val;
            break;
    }

    // Don't free the old value if we're returning the same value (identity conversion or passthrough)
    if (result.string != val.string || result.type != val.type) {
        RValue_free(&val);
    }

    // Set gmlStackType to the destination type so Dup can compute correct byte sizes (BC17+ only)
#if IS_BC17_OR_HIGHER_ENABLED
    if (IS_BC17_OR_HIGHER(ctx)) {
        result.gmlStackType = dstType;
    }
#endif
    stackPush(ctx, result);
}

// Tries to parse a string as a real number, mirroring HTML5 yyCompareVal's behavior:
// trim leading whitespace, then accept a numeric prefix (sign, digits, decimal, exponent).
// Returns true on success, with the parsed value written to *out.
static bool tryParseRealFromString(const char* str, GMLReal* out) {
    if (str == nullptr) return false;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    if (*str == '\0') return false;
    char* endPtr = nullptr;
    GMLReal value = GMLReal_strtod(str, &endPtr);
    if (endPtr == str) return false;
    *out = value;
    return true;
}

static void handleCmp(VMContext* ctx, uint32_t instr) {
    uint8_t cmpKind = instrCmpKind(instr);
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);

    bool result;
    if (a.type == RVALUE_UNDEFINED || b.type == RVALUE_UNDEFINED) {
        // Undefined is only == to undefined
        bool eq = a.type == b.type;
        switch (cmpKind) {
            case CMP_EQ:  result = eq;  break;
            case CMP_NEQ: result = !eq; break;
            default:      result = false; break;
        }
    } else if (a.type == RVALUE_ARRAY || b.type == RVALUE_ARRAY) {
        // Array is only == to the same array
        bool eq = (a.type == RVALUE_ARRAY && b.type == RVALUE_ARRAY) && (a.array == b.array);
        switch (cmpKind) {
            case CMP_EQ:  result = eq;  break;
            case CMP_NEQ: result = !eq; break;
            default:      result = false; break;
        }
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (a.type == RVALUE_METHOD || b.type == RVALUE_METHOD) {
        // Method is only == to the same method
        bool eq = (a.type == RVALUE_METHOD && b.type == RVALUE_METHOD) && (a.method == b.method);
        switch (cmpKind) {
            case CMP_EQ:  result = eq;  break;
            case CMP_NEQ: result = !eq; break;
            default:      result = false; break;
        }
#endif
    } else if (a.type == RVALUE_STRUCT || b.type == RVALUE_STRUCT) {
        // Struct is only == to the same struct (identity comparison)
        bool eq = (a.type == RVALUE_STRUCT && b.type == RVALUE_STRUCT) && (a.structInst == b.structInst);
        switch (cmpKind) {
            case CMP_EQ:  result = eq;  break;
            case CMP_NEQ: result = !eq; break;
            default:      result = false; break;
        }
    } else if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        int cmp = strcmp(a.string != nullptr ? a.string : "", b.string != nullptr ? b.string : "");
        switch (cmpKind) {
            case CMP_LT:  result = 0 > cmp; break;
            case CMP_LTE: result = 0 >= cmp; break;
            case CMP_EQ:  result = cmp == 0; break;
            case CMP_NEQ: result = cmp != 0; break;
            case CMP_GTE: result = cmp >= 0; break;
            case CMP_GT:  result = cmp > 0; break;
            default: result = false; break;
        }
    } else {
        // Mixed string/number: coerce strings to reals (matching GameMaker-HTML5 yyCompareVal).
        // Don't be fooled, this behavior is not a GameMaker-HTML5 (JavaScript) quirk! Some GameMaker games do use this,
        // such as gml_Object_obj_ch2_scene6_Step_0 in DELTARUNE: Chapter 2, where the c_wait uses a string instead of a number
        //
        // If a string side fails to parse as a number, the values are considered incomparable: false for all comparisons except NEQ.
        bool incomparable = false;
        GMLReal da = 0.0;
        GMLReal db = 0.0;
        if (a.type == RVALUE_STRING) {
            if (!tryParseRealFromString(a.string, &da)) incomparable = true;
        } else {
            da = RValue_toReal(a);
        }
        if (!incomparable) {
            if (b.type == RVALUE_STRING) {
                if (!tryParseRealFromString(b.string, &db)) incomparable = true;
            } else {
                db = RValue_toReal(b);
            }
        }

        if (incomparable) {
            switch (cmpKind) {
                case CMP_EQ:  result = false; break;
                case CMP_NEQ: result = true;  break;
                default:      result = false; break;
            }
        } else {
            GMLReal diff = da - db;
            // GML uses epsilon-based comparison for all numeric CMP operations
            int cmp = GMLReal_fabs(diff) <= GML_MATH_EPSILON ? 0 : (diff < 0 ? -1 : 1);
            switch (cmpKind) {
                case CMP_LT:  result = cmp < 0; break;
                case CMP_LTE: result = cmp <= 0; break;
                case CMP_EQ:  result = cmp == 0; break;
                case CMP_NEQ: result = cmp != 0; break;
                case CMP_GTE: result = cmp >= 0; break;
                case CMP_GT:  result = cmp > 0; break;
                default: result = false; break;
            }
        }
    }

    RValue_free(&a);
    RValue_free(&b);
    stackPush(ctx,RValue_makeBool(result));
}

#if IS_BC17_OR_HIGHER_ENABLED
// Converts a native byte count to RValue slot count by walking the stack backwards from a given position.
// Only used by BC17+ Dup paths; reads the per-slot gmlStackType which doesn't exist on BC16-only builds.
static int32_t bytesToSlotCount(VMContext* ctx, int32_t nativeBytes, int32_t stackPos) {
    int32_t slots = 0;
    int32_t remaining = nativeBytes;
    while (remaining > 0) {
        slots++;
        require(stackPos >= slots);
        uint8_t slotGmlType = ctx->stack.slots[stackPos - slots].gmlStackType;
        remaining -= gmlTypeNativeSize(slotGmlType);
    }
    require(remaining == 0); // Byte count must align exactly to slot boundaries
    return slots;
}
#endif

static void handleDup(VMContext* ctx, uint32_t instr) {
    uint16_t operand = (uint16_t)(instr & 0xFFFF);
#if IS_BC17_OR_HIGHER_ENABLED
    uint8_t type1 = instrType1(instr);
    int32_t typeSize = gmlTypeNativeSize(type1);

    // Swap mode: bit 15 of operand is set
    // The Dup instruction doubles as a stack rotation when bit 15 is set.
    // It takes the top N items and moves them below the next M items.
    // Bits 0-10: top group size (in native type units)
    // Bits 11-14: bottom group size (in native type units)
    if (IS_BC17_OR_HIGHER(ctx) && (operand & 0x8000) != 0) {
        int32_t topNativeCount = operand & 0x7FF;
        int32_t bottomNativeCount = (operand >> 11) & 0xF;
        int32_t topBytes = topNativeCount * typeSize;
        int32_t bottomBytes = bottomNativeCount * typeSize;

        // Convert byte counts to slot counts
        int32_t topSlots = bytesToSlotCount(ctx, topBytes, ctx->stack.top);
        int32_t bottomSlots = bytesToSlotCount(ctx, bottomBytes, ctx->stack.top - topSlots);

        int32_t totalSlots = topSlots + bottomSlots;
        int32_t baseIdx = ctx->stack.top - totalSlots;

        // Save top group to temp
        RValue temp[32];
        for (int32_t i = 0; topSlots > i; i++) {
            temp[i] = ctx->stack.slots[ctx->stack.top - topSlots + i];
        }

        // Shift bottom group up to where top group was
        for (int32_t i = bottomSlots - 1; i >= 0; i--) {
            ctx->stack.slots[baseIdx + topSlots + i] = ctx->stack.slots[baseIdx + i];
        }

        // Place top group at the bottom
        for (int32_t i = 0; topSlots > i; i++) {
            ctx->stack.slots[baseIdx + i] = temp[i];
        }
        return;
    }
#endif

    // Normal dup mode
    int32_t count;

#if IS_BC17_OR_HIGHER_ENABLED
    if (IS_BC17_OR_HIGHER(ctx)) {
        // In bytecode 17+, the operand encodes a native element count: total bytes = (operand + 1) * typeSize(type1).
        // The native runner's stack stores raw bytes (int=4, double=8, variable=16), but our VM uses uniform RValue slots.
        // We walk backward through the stack, summing each slot's native size (tracked via gmlStackType), to find how many slots correspond to the byte count.
        int32_t totalBytes = ((int32_t)(operand & 0x7FFF) + 1) * typeSize;

        count = bytesToSlotCount(ctx, totalBytes, ctx->stack.top);
    } else {
        // Bytecode 16: operand directly encodes how many additional items beyond 1 to duplicate (dup.i 0 = duplicate 1 item, dup.i 1 = duplicate 2 items, etc)
        count = (int32_t)(operand & 0xFF) + 1;
        require(ctx->stack.top >= count);
    }
#else
    // Bytecode 16: operand directly encodes how many additional items beyond 1 to duplicate
    count = (int32_t)(operand & 0xFF) + 1;
    require(ctx->stack.top >= count);
#endif

    // Copy 'count' items from the top of the stack (preserving order)
    int32_t startIdx = ctx->stack.top - count;
    for (int32_t i = 0; count > i; i++) {
        RValue copy = ctx->stack.slots[startIdx + i];

        // If the value owns a string, duplicate it to avoid double-free.
        // For arrays and methods, bump the refcount so each duplicate independently owns a reference.
        if (copy.type == RVALUE_STRING && copy.ownsReference && copy.string != nullptr) {
            copy.string = safeStrdup(copy.string);
        } else if (copy.type == RVALUE_ARRAY && copy.ownsReference && copy.array != nullptr) {
            GMLArray_incRef(copy.array);
#if IS_BC17_OR_HIGHER_ENABLED
        } else if (copy.type == RVALUE_METHOD && copy.ownsReference && copy.method != nullptr) {
            GMLMethod_incRef(copy.method);
#endif
        } else if (copy.type == RVALUE_STRUCT && copy.ownsReference && copy.structInst != nullptr) {
            Instance_structIncRef(copy.structInst);
        }

        stackPush(ctx, copy);
    }
}

// ===[ Function Call Handler ]===

static void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    int32_t argCount = instr & 0xFFFF;
    uint32_t funcIndex = resolveFuncOperand(extraData);
    require(ctx->dataWin->func.functionCount > funcIndex);

    // Pop arguments from stack (args pushed right-to-left, so first arg is on top)
    // Use stack-allocated buffer for small arg counts (GMS 1.4 supports up to 16 arguments)
    RValue stackArgs[GML_MAX_ARGUMENTS];
    RValue* args = nullptr;
    if (argCount > 0) {
        args = (GML_MAX_ARGUMENTS >= argCount) ? stackArgs : safeMalloc(argCount * sizeof(RValue));
        repeat(argCount, i) {
            args[i] = stackPop(ctx);
        }
    }

#ifdef ENABLE_VM_TRACING
    const char* funcName = ctx->dataWin->func.functions[funcIndex].name;
    bool functionIsBeingTraced = shgeti(ctx->functionCallsToBeTraced, "*") != -1 || shgeti(ctx->functionCallsToBeTraced, funcName) != -1 || shgeti(ctx->functionCallsToBeTraced, ctx->currentCodeName) != -1;
    char* functionArgumentList = nullptr;
    if (functionIsBeingTraced) {
        functionArgumentList = safeStrdup("");
        for (int32_t i = 0; i < argCount; i++) {
            char* display = RValue_toStringFancy(args[i]);

            if (i > 0) {
                char* tmp = safeMalloc(strlen(functionArgumentList) + 2 + strlen(display) + 1);
                sprintf(tmp, "%s, %s", functionArgumentList, display);
                free(functionArgumentList);
                functionArgumentList = tmp;
            } else {
                free(functionArgumentList);
                functionArgumentList = safeStrdup(display);
            }
            free(display);
        }

        fprintf(stderr, "VM: [%s] Calling function \"%s(%s)\"\n", ctx->currentCodeName, funcName, functionArgumentList);
    }
#endif

    // Use cached function resolution to avoid per-call string hash lookups
    FuncCallCache* cache = &ctx->funcCallCache[funcIndex];

    // Fast path: cached builtin function pointer
    if (cache->builtin != nullptr) {
        BuiltinFunc builtin = (BuiltinFunc) cache->builtin;
        RValue result = builtin(ctx, args, argCount);
        // Free arguments
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            if (args != stackArgs) free(args);
        }

#ifdef ENABLE_VM_TRACING
        if (functionIsBeingTraced) {
            char* returnValueAsString = RValue_toStringFancy(result);
            fprintf(stderr, "VM: [%s] Built-in function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
            free(returnValueAsString);
            free(functionArgumentList);
        }
#endif

        stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
        return;
    }

    // Fast path: cached script code index
    if (cache->scriptCodeIndex >= 0) {
        RValue result = VM_callCodeIndex(ctx, cache->scriptCodeIndex, args, argCount);

#ifdef ENABLE_VM_TRACING
        if (functionIsBeingTraced) {
            char* returnValueAsString = RValue_toStringFancy(result);
            fprintf(stderr, "VM: [%s] Script function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
            free(returnValueAsString);
            free(functionArgumentList);
        }
#endif

        // Free arguments (VM_callCodeIndex copies what it needs)
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            if (args != stackArgs) free(args);
        }

        stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
        return;
    }

    // Slow path: unknown function (not cached as builtin or script)
#ifdef ENABLE_VM_STUB_LOGS
    const char* unknownFuncName = ctx->dataWin->func.functions[funcIndex].name;

    // Log once per (callingCode, funcName) pair
    const char* callerName = VM_getCallerName(ctx);
    char* dedupKey = VM_createDedupKey(callerName, unknownFuncName);

    if (ctx->alwaysLogUnknownFunctions || 0 > shgeti(ctx->loggedUnknownFuncs, dedupKey)) {
        shput(ctx->loggedUnknownFuncs, dedupKey, true);
        fprintf(stderr, "VM: [%s] Unknown function \"%s\"!\n", callerName, unknownFuncName);
    } else {
        free(dedupKey);
    }
#endif

    // Free arguments and push undefined
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        if (args != stackArgs) free(args);
    }

#ifdef ENABLE_VM_TRACING
    if (functionIsBeingTraced) {
        free(functionArgumentList);
    }
#endif

    stackPush(ctx, RValue_makeUndefined());
}

#if IS_BC17_OR_HIGHER_ENABLED
// BC17+ CALLV: dynamic call through a variable (method/script reference).
// Stack layout (top -> bottom): function, instance, arg[N-1], ..., arg[0]
// argCount is in the low 16 bits of the instruction.
static void handleCallV(VMContext* ctx, uint32_t instr) {
    int32_t argCount = instr & 0xFFFF;

    RValue function = stackPop(ctx);
    RValue instance = stackPop(ctx);

    RValue stackArgs[GML_MAX_ARGUMENTS];
    RValue* args = nullptr;
    if (argCount > 0) {
        args = (GML_MAX_ARGUMENTS >= argCount) ? stackArgs : safeMalloc(argCount * sizeof(RValue));
        repeat(argCount, i) {
            args[i] = stackPop(ctx);
        }
    }

    int32_t codeIndex = -1;
    int32_t boundInstance = -1;
    BuiltinFunc builtin = nullptr;
    const char* unresolvedName = nullptr;
    if (function.type == RVALUE_METHOD && function.method != nullptr) {
        codeIndex = function.method->codeIndex;
        boundInstance = function.method->boundInstanceId;
        builtin = (BuiltinFunc) function.method->builtin;
        unresolvedName = function.method->unresolvedName;
    }

    // Decide target self: prefer method's bound instance, else the stack-provided instance.
    int32_t targetInstance = (boundInstance > 0) ? boundInstance : RValue_toInt32(instance);
    Instance* savedSelf = ctx->currentInstance;
    if (targetInstance != INSTANCE_SELF && targetInstance != 0) {
        Instance* target = findInstanceByTarget(ctx, targetInstance);
        if (target != nullptr) ctx->currentInstance = target;
    }

    RValue result;
    if (codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex) {
        result = VM_callCodeIndex(ctx, codeIndex, args, argCount);
    } else if (builtin != nullptr) {
        result = builtin(ctx, args, argCount);
    } else if (unresolvedName != nullptr) {
#ifdef ENABLE_VM_STUB_LOGS
        const char* callerName = VM_getCallerName(ctx);
        char* dedupKey = VM_createDedupKey(callerName, unresolvedName);
        if (ctx->alwaysLogUnknownFunctions || 0 > shgeti(ctx->loggedUnknownFuncs, dedupKey)) {
            shput(ctx->loggedUnknownFuncs, dedupKey, true);
            fprintf(stderr, "VM: [%s] Unknown function \"%s\"! (via CallV)\n", callerName, unresolvedName);
        } else {
            free(dedupKey);
        }
#endif
        result = RValue_makeUndefined();
    } else {
        fprintf(stderr, "VM: [%s] CALLV with unresolvable function reference (type=%d, codeIndex=%d)\n", ctx->currentCodeName, function.type, codeIndex);
        result = RValue_makeUndefined();
    }

    ctx->currentInstance = savedSelf;

    RValue_free(&function);
    RValue_free(&instance);
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        if (args != stackArgs) free(args);
    }

    stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
}
#endif

// ===[ With-Statement Helpers (PushEnv/PopEnv) ]===

// Checks if objectIndex is or inherits from targetObjectIndex by walking the parent chain.
bool VM_isObjectOrDescendant(DataWin* dataWin, int32_t objectIndex, int32_t targetObjectIndex) {
    int32_t currentObj = objectIndex;
    int depth = 0;
    while (currentObj >= 0 && (uint32_t) currentObj < dataWin->objt.count && 32 > depth) {
        if (currentObj == targetObjectIndex) return true;
        currentObj = dataWin->objt.objects[currentObj].parentId;
        depth++;
    }
    return false;
}


// Sets the VM instance context from an Instance.
static void switchToInstance(VMContext* ctx, Instance* inst) {
    ctx->currentInstance = inst;
}

// Restores VM context from an EnvFrame's saved fields.
static void restoreEnvContext(VMContext* ctx, EnvFrame* frame) {
    ctx->currentInstance = frame->savedInstance;
    ctx->otherInstance = frame->savedOtherInstance;
}

static void handlePushEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    int32_t jumpOffset = instrJumpOffset(instr);

    // Pop target from stack
    int32_t target = stackPopInt32(ctx);
    // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real target"
    if (IS_BC17_OR_HIGHER(ctx) && target == INSTANCE_STACKTOP) {
        target = resolveInstanceStackTop(ctx);
    }

    // Create env frame, save current context
    EnvFrame* frame = safeMalloc(sizeof(EnvFrame));
    frame->savedInstance = (Instance*) ctx->currentInstance;
    frame->savedOtherInstance = (Instance*) ctx->otherInstance;
    frame->instanceList = nullptr;
    frame->currentIndex = 0;
    frame->parent = ctx->envStack;
    ctx->envStack = frame;

    // Inside a with-block, "other" refers to the instance that executed the with-statement
    ctx->otherInstance = (Instance*) ctx->currentInstance;

    Runner* runner = (Runner*) ctx->runner;

    if (target == INSTANCE_SELF) {
        // with(self) - no-op, keep current instance
        return;
    }

    if (target == INSTANCE_OTHER) {
        // with(other) - switch to the instance that was "self" before the nearest enclosing with-block
        // For nested with-blocks, other refers to the saved instance from the parent env frame
        if (frame->parent != nullptr) {
            switchToInstance(ctx, frame->parent->savedInstance);
        } else if (ctx->otherInstance != nullptr) {
            // No parent env frame, but we have an otherInstance (e.g., from collision events)
            switchToInstance(ctx, (Instance*) ctx->otherInstance);
        }
        // If no parent frame and no otherInstance, keep the saved instance (no-op)
        return;
    }

    if (target == INSTANCE_NOONE) {
        // with(noone) - skip the block entirely
        ctx->ip = instrAddr + jumpOffset;
        return;
    }

    if (target == INSTANCE_ALL) {
        // with(all) - iterate over all active instances
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; instanceCount > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active) {
                arrput(frame->instanceList, inst);
            }
        }

        if (arrlen(frame->instanceList) == 0) {
            // No active instances, skip the block
            ctx->ip = instrAddr + jumpOffset;
            return;
        }

        frame->currentIndex = 0;
        switchToInstance(ctx, frame->instanceList[0]);
        return;
    }

    if (target >= 0 && 100000 > target) {
        // Object index - copy the descendant-inclusive list for this object into the frame's own list. frame->instanceList has with-block lifetime (not the snapshot arena's loop lifetime), so we don't use the forEach macro; we just copy directly and filter "active" to match prior semantics (deactivated instances are skipped).
        if (ctx->dataWin->objt.count > (uint32_t) target) {
            Instance** source = runner->instancesByObject[target];
            int32_t sourceCount = (int32_t) arrlen(source);
            for (int32_t i = 0; sourceCount > i; i++) {
                Instance* inst = source[i];
                if (inst->active) arrput(frame->instanceList, inst);
            }
        }

        if (arrlen(frame->instanceList) == 0) {
            // No matching instances, skip the block
            ctx->ip = instrAddr + jumpOffset;
            return;
        }

        frame->currentIndex = 0;
        switchToInstance(ctx, frame->instanceList[0]);
        return;
    }

    if (target >= 100000) {
        // Instance ID - find specific instance
        Instance* inst = hmget(runner->instancesToId, target);
        if (inst != nullptr && inst->active) {
            switchToInstance(ctx, inst);
            return;
        }

        // Instance not found, skip the block
        ctx->ip = instrAddr + jumpOffset;
        return;
    }

    fprintf(stderr, "VM: PushEnv with unhandled target %d\n", target);
    ctx->ip = instrAddr + jumpOffset;
}

static void handlePopEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    EnvFrame* frame = ctx->envStack;
    require(frame != nullptr);

    // Check for exit magic: PopEnv with 0xF00000 operand means "unwind env stack and exit/return"
    if ((instr & 0x00FFFFFF) == 0xF00000) {
        // Restore context and pop frame
        restoreEnvContext(ctx, frame);
        ctx->envStack = frame->parent;
        arrfree(frame->instanceList);
        free(frame);
        return;
    }

    // Check if there are more instances to iterate
    if (frame->instanceList != nullptr && arrlen(frame->instanceList) > frame->currentIndex + 1) {
        frame->currentIndex++;
        Instance* nextInst = frame->instanceList[frame->currentIndex];
        // Skip destroyed instances
        while (!nextInst->active && arrlen(frame->instanceList) > frame->currentIndex + 1) {
            frame->currentIndex++;
            nextInst = frame->instanceList[frame->currentIndex];
        }
        if (nextInst->active) {
            switchToInstance(ctx, nextInst);
            // Jump back to the start of the with-block body
            int32_t jumpOffset = instrJumpOffset(instr);
            ctx->ip = instrAddr + jumpOffset;
            return;
        }
    }

    // Done iterating - restore context and pop frame
    restoreEnvContext(ctx, frame);
    ctx->envStack = frame->parent;
    arrfree(frame->instanceList);
    free(frame);
}

// ===[ Execution Loop ]===

static const char* opcodeName(uint8_t opcode) {
    switch (opcode) {
        case OP_CONV:    return "Conv";
        case OP_MUL:     return "Mul";
        case OP_DIV:     return "Div";
        case OP_REM:     return "Rem";
        case OP_MOD:     return "Mod";
        case OP_ADD:     return "Add";
        case OP_SUB:     return "Sub";
        case OP_AND:     return "And";
        case OP_OR:      return "Or";
        case OP_XOR:     return "Xor";
        case OP_NEG:     return "Neg";
        case OP_NOT:     return "Not";
        case OP_SHL:     return "Shl";
        case OP_SHR:     return "Shr";
        case OP_CMP:     return "Cmp";
        case OP_POP:     return "Pop";
        case OP_PUSHI:   return "PushI";
        case OP_DUP:     return "Dup";
        case OP_RET:     return "Ret";
        case OP_EXIT:    return "Exit";
        case OP_POPZ:    return "Popz";
        case OP_B:       return "B";
        case OP_BT:      return "BT";
        case OP_BF:      return "BF";
        case OP_PUSHENV: return "PushEnv";
        case OP_POPENV:  return "PopEnv";
        case OP_PUSH:    return "Push";
        case OP_PUSHLOC: return "PushLoc";
        case OP_PUSHGLB: return "PushGlb";
        case OP_PUSHBLTN:return "PushBltn";
        case OP_CALL:    return "Call";
        case OP_CALLV:   return "CallV";
        case OP_BREAK:   return "Break";
        default:         return "???";
    }
}

#ifdef ENABLE_VM_OPCODE_PROFILER
static char gmlTypeChar(uint8_t type);

static const char* rvalueTypeName(uint8_t type) {
    switch (type) {
        case RVALUE_REAL:      return "REAL";
        case RVALUE_STRING:    return "STRING";
        case RVALUE_INT32:     return "INT32";
        case RVALUE_INT64:     return "INT64";
        case RVALUE_BOOL:      return "BOOL";
        case RVALUE_UNDEFINED: return "UNDEF";
        case RVALUE_ARRAY:     return "ARRAY";
        case RVALUE_METHOD:    return "METHOD";
        case RVALUE_STRUCT:    return "STRUCT";
        case 0xF:              return "-";
        default:               return "???";
    }
}

static const char* breakSubOpName(int16_t breakType) {
    switch (breakType) {
        case BREAK_CHKINDEX:    return "chkindex";
        case BREAK_PUSHAF:      return "pushaf";
        case BREAK_POPAF:       return "popaf";
        case BREAK_PUSHAC:      return "pushac";
        case BREAK_SETOWNER:    return "setowner";
        case BREAK_ISSTATICOK:  return "isstaticok";
        case BREAK_SETSTATIC:   return "setstatic";
        case BREAK_SAVEAREF:    return "savearef";
        case BREAK_RESTOREAREF: return "restorearef";
        default:                return "???";
    }
}

void VM_printOpcodeProfilerReport(const VMContext* ctx) {
    if (!ctx->opcodeProfilerEnabled) return;

    typedef struct { uint16_t key; uint64_t count; } CountEntry;
    CountEntry entries[256];
    int entryCount = 0;
    uint64_t total = 0;
    for (int i = 0; 256 > i; i++) {
        if (ctx->opcodeCounts[i] > 0) {
            entries[entryCount].key = (uint16_t) i;
            entries[entryCount].count = ctx->opcodeCounts[i];
            entryCount++;
            total += ctx->opcodeCounts[i];
        }
    }

    // Simple insertion sort (max 256 entries, runs once at shutdown)
    for (int i = 1; entryCount > i; i++) {
        CountEntry tmp = entries[i];
        int j = i;
        while (j > 0 && entries[j - 1].count < tmp.count) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }

    fprintf(stderr, "=== Opcode Profiler Report ===\n");
    fprintf(stderr, "Total instructions executed: %llu\n", (unsigned long long) total);
    fprintf(stderr, "%-12s %-6s %16s %8s\n", "Opcode", "Hex", "Count", "Pct");
    forEachIndexed(CountEntry, entry, i, entries, entryCount) {
        (void) i;
        double pct = total > 0 ? (100.0 * (double) entry->count / (double) total) : 0.0;
        fprintf(stderr, "%-12s 0x%02X   %16llu %7.2f%%\n", opcodeName((uint8_t) entry->key), (uint8_t) entry->key, (unsigned long long) entry->count, pct);
    }

    // Per-opcode breakdown by type variant. Sorted within each opcode by count desc.
    fprintf(stderr, "\n--- Type variant breakdown (per opcode) ---\n");
    forEachIndexed(CountEntry, entry, idx, entries, entryCount) {
        (void) idx;
        uint8_t opcode = (uint8_t) entry->key;
        const uint64_t* variants = &ctx->opcodeVariantCounts[opcode * 256];

        CountEntry variantEntries[256];
        int variantCount = 0;
        for (int t = 0; 256 > t; t++) {
            if (variants[t] > 0) {
                variantEntries[variantCount].key = (uint16_t) t;
                variantEntries[variantCount].count = variants[t];
                variantCount++;
            }
        }
        for (int i = 1; variantCount > i; i++) {
            CountEntry tmp = variantEntries[i];
            int j = i;
            while (j > 0 && variantEntries[j - 1].count < tmp.count) {
                variantEntries[j] = variantEntries[j - 1];
                j--;
            }
            variantEntries[j] = tmp;
        }

        fprintf(stderr, "%s (0x%02X): %llu total\n", opcodeName(opcode), opcode, (unsigned long long) entry->count);
        forEachIndexed(CountEntry, ve, vi, variantEntries, variantCount) {
            (void) vi;
            uint8_t type1 = (uint8_t) ((ve->key >> 4) & 0xF);
            uint8_t type2 = (uint8_t) (ve->key & 0xF);
            double vpct = entry->count > 0 ? (100.0 * (double) ve->count / (double) entry->count) : 0.0;
            fprintf(stderr, "    .%c.%c  %16llu %7.2f%%\n", gmlTypeChar(type1), gmlTypeChar(type2), (unsigned long long) ve->count, vpct);
        }

        // Runtime RValue type breakdown (a, b types observed at execution time)
        {
            const uint64_t* rvCounts = &ctx->opcodeRValueTypeCounts[opcode * 256];
            CountEntry rvEntries[256];
            int rvCount = 0;
            uint64_t rvTotal = 0;
            for (int t = 0; 256 > t; t++) {
                if (rvCounts[t] > 0) {
                    rvEntries[rvCount].key = (uint16_t) t;
                    rvEntries[rvCount].count = rvCounts[t];
                    rvCount++;
                    rvTotal += rvCounts[t];
                }
            }
            if (rvCount > 0) {
                for (int i = 1; rvCount > i; i++) {
                    CountEntry tmp = rvEntries[i];
                    int j = i;
                    while (j > 0 && rvEntries[j - 1].count < tmp.count) {
                        rvEntries[j] = rvEntries[j - 1];
                        j--;
                    }
                    rvEntries[j] = tmp;
                }
                fprintf(stderr, "    -- runtime types (a, b):\n");
                forEachIndexed(CountEntry, re, ri, rvEntries, rvCount) {
                    (void) ri;
                    uint8_t typeA = (uint8_t) ((re->key >> 4) & 0xF);
                    uint8_t typeB = (uint8_t) (re->key & 0xF);
                    double rpct = rvTotal > 0 ? (100.0 * (double) re->count / (double) rvTotal) : 0.0;
                    fprintf(stderr, "    (%-6s, %-6s) %16llu %7.2f%%\n", rvalueTypeName(typeA), rvalueTypeName(typeB), (unsigned long long) re->count, rpct);
                }
            }
        }

        // Extended BREAK (0xFF) sub-opcode breakdown
        if (opcode == OP_BREAK) {
            CountEntry breakEntries[64];
            int breakCount = 0;
            for (int i = 0; 64 > i; i++) {
                if (ctx->breakSubOpCounts[i] > 0) {
                    breakEntries[breakCount].key = (uint16_t) i;
                    breakEntries[breakCount].count = ctx->breakSubOpCounts[i];
                    breakCount++;
                }
            }
            for (int i = 1; breakCount > i; i++) {
                CountEntry tmp = breakEntries[i];
                int j = i;
                while (j > 0 && breakEntries[j - 1].count < tmp.count) {
                    breakEntries[j] = breakEntries[j - 1];
                    j--;
                }
                breakEntries[j] = tmp;
            }
            fprintf(stderr, "    -- sub-opcodes:\n");
            forEachIndexed(CountEntry, be, bi, breakEntries, breakCount) {
                (void) bi;
                int16_t breakType = (int16_t) -((int) be->key);
                double bpct = entry->count > 0 ? (100.0 * (double) be->count / (double) entry->count) : 0.0;
                fprintf(stderr, "    %-12s (%4d) %16llu %7.2f%%\n", breakSubOpName(breakType), (int) breakType, (unsigned long long) be->count, bpct);
            }
        }
    }
    fprintf(stderr, "==============================\n");
}
#endif // ENABLE_VM_OPCODE_PROFILER

// Forward declaration for formatInstruction (defined in disassembler section, used by trace-opcodes)
static void formatInstruction(VMContext* ctx, const uint8_t* bytecodeBase, uint32_t instrAddr, uint32_t instr, const uint8_t* extraData, char* opcodeStr, size_t opcodeSize, char* operandStr, size_t operandSize, char* commentStr, size_t commentSize);

#if IS_BC17_OR_HIGHER_ENABLED
// ===[ BREAK sub-opcode handlers (BC17+) ]===

static void handleBreakChkIndex(VMContext* ctx, uint32_t instrAddr) {
    // Validate top-of-stack array index is in [0, 32000)
    RValue* top = stackPeek(ctx);
    int32_t idx = RValue_toInt32(*top);
    if (0 > idx || 32000 <= idx) {
        fprintf(stderr, "VM: chkindex out of bounds: %d at offset %u in %s\n", idx, instrAddr, ctx->currentCodeName);
        abort();
    }
}

static void handleBreakPushAF(VMContext* ctx) {
    // Pop index + array ref, push array[index]. Array ref is a weak RVALUE_ARRAY pointer.
    int32_t idx = stackPopInt32(ctx);
    RValue arrayRef = stackPop(ctx);
    RValue result;
    RValue* cell = arrayRef.type == RVALUE_ARRAY ? GMLArray_slot(arrayRef.array, idx) : nullptr;
    if (cell != nullptr) {
        result = *cell;
        result.ownsReference = false; // weak view
    } else {
        result = (RValue){ .type = RVALUE_UNDEFINED };
    }
    stackPush(ctx, result);
    RValue_free(&arrayRef);
}

static void handleBreakPopAF(VMContext* ctx) {
    // Pop index + array ref + value, store value at array[index].
    // CoW via VM_arrayWriteAt requires a slot pointer, since the stack-held arrayRef is a weak view, the real slot is whatever variable holds this array.
    // We can't easily recover the slot here, so we write directly into the array (no CoW fork at this level, fork already happened when the top-level variable was first written, or on a PUSHAC materialisation).
    // Assert the array is uniquely-owned or matches the current scope owner. A mismatch here means a shared/aliased array is about to be mutated in place, which silently breaks CoW semantics. BC17+ default mode (pass by reference) is expected to satisfy this since fork already happened at the top-level write. If this fires, a CoW path upstream failed to fork.
    int32_t idx = stackPopInt32(ctx);
    RValue arrayRef = stackPop(ctx);
    RValue value = stackPop(ctx);
    if (arrayRef.type == RVALUE_ARRAY && arrayRef.array != nullptr && idx >= 0) {
        GMLArray* arr = arrayRef.array;
        requireMessage(arr->refCount == 1 || arr->owner == ctx->currentArrayOwner, "BREAK_POPAF: Writing through shared/aliased array without prior CoW fork");
        GMLArray_growTo(arr, idx + 1);
        storeIntoArraySlot(GMLArray_slot(arr, idx), value);
    }
    RValue_free(&arrayRef);
    RValue_free(&value);
}

static void handleBreakPushAC(VMContext* ctx, uint32_t instrAddr) {
    // Pop index + parent array ref, push sub-array at parent[index]. Materialise a fresh sub-array if the slot isn't already an RVALUE_ARRAY (multi-dim auto-init).
    int32_t idx = stackPopInt32(ctx);
    RValue arrayRef = stackPop(ctx);
    if (arrayRef.type != RVALUE_ARRAY || arrayRef.array == nullptr) {
        fprintf(stderr, "VM: pushac on non-array (type=%d) at offset %u in %s\n", arrayRef.type, instrAddr, ctx->currentCodeName);
        abort();
    }
    GMLArray* parent = arrayRef.array;
    GMLArray_growTo(parent, idx + 1);
    RValue* parentSlot = GMLArray_slot(parent, idx);
    if (parentSlot->type != RVALUE_ARRAY || parentSlot->array == nullptr) {
        RValue_free(parentSlot);
        GMLArray* sub = GMLArray_create(0);
        sub->owner = parent->owner;
        *parentSlot = (RValue){ .array = sub, .type = RVALUE_ARRAY, .ownsReference = true, RVALUE_INIT_GMLTYPE(GML_TYPE_VARIABLE) };
    }
    stackPush(ctx, RValue_makeArrayWeak(parentSlot->array));
    RValue_free(&arrayRef);
}

static void handleBreakSetOwner(VMContext* ctx) {
    // CoW scope owner for BC17+.
    // The bytecode emits this at the top of each script or event, passing a token (usually self-instance ID cast to int) that uniquely identifies the current scope.
    // Arrays whose .owner doesn't match fork on write.
    RValue value = stackPop(ctx);
    int64_t token = RValue_toInt64(value);
    ctx->currentArrayOwner = (void*) (intptr_t) token;
    RValue_free(&value);
}

static void handleBreakIsStaticOk(VMContext* ctx) {
    // Push bool: has this function's static block already run?
    bool initialized = ctx->staticInitialized[ctx->currentCodeIndex];
    stackPush(ctx, RValue_makeBool(initialized));
}

static void handleBreakSetStatic(VMContext* ctx) {
    // Mark current function's static as initialized
    ctx->staticInitialized[ctx->currentCodeIndex] = true;
}

static void handleBreakSaveARef(VMContext* ctx) {
    // Native 2.3: SAVEAREF does `g_pSavedArraySetContainer = g_pArraySetContainer`, doesn't touch the stack.
    // `g_pArraySetContainer` is a runner-global set by PUSHAC when traversing multi-dim parents, used by SET_RValue_Array as the container to write into.
    // Since our PUSHAC pushes the sub-array directly onto the VM stack instead of stashing it in a container, this is a no-op.
    //
    // To track if we are doing everything correct, we'll track the savearefBalance to figure out when a game does something wrong.
    ctx->savearefBalance++;
}

static void handleBreakRestoreARef(VMContext* ctx) {
    // Native 2.3: restores `g_pArraySetContainer` from the saved slot. No-op here (see BREAK_SAVEAREF).
    // A negative balance means RESTOREAREF was emitted without a matching SAVEAREF, which means that we are doing things wrong or it is a bytecode pattern that we don't understand.
    requireMessage(ctx->savearefBalance > 0, "BREAK_RESTOREAREF without matching SAVEAREF");
    ctx->savearefBalance--;
}

static void handleBreak(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    if (IS_BC16_OR_BELOW(ctx)) return;
    int16_t breakType = instrInstanceType(instr);
    switch (breakType) {
        case BREAK_CHKINDEX:    handleBreakChkIndex(ctx, instrAddr); break;
        case BREAK_PUSHAF:      handleBreakPushAF(ctx); break;
        case BREAK_POPAF:       handleBreakPopAF(ctx); break;
        case BREAK_PUSHAC:      handleBreakPushAC(ctx, instrAddr); break;
        case BREAK_SETOWNER:    handleBreakSetOwner(ctx); break;
        case BREAK_ISSTATICOK:  handleBreakIsStaticOk(ctx); break;
        case BREAK_SETSTATIC:   handleBreakSetStatic(ctx); break;
        case BREAK_SAVEAREF:    handleBreakSaveARef(ctx); break;
        case BREAK_RESTOREAREF: handleBreakRestoreARef(ctx); break;
        default:
            fprintf(stderr, "VM: Unknown BREAK sub-opcode %d at offset %u in %s\n", breakType, instrAddr, ctx->currentCodeName);
            abort();
    }
}
#endif

#define VM_SYNC_IP()    do { ctx->ip = ip; } while (0)
#define VM_RELOAD_IP()  do { ip = ctx->ip; } while (0)

static RValue executeLoop(VMContext* ctx) {
    // codeEnd and bytecodeBase are invariant for the lifetime of this executeLoop call, so let's hoist them to avoid the compiler emitting code to
    // reload the values at the end of every iteration.
    const uint32_t codeEnd = ctx->codeEnd;
    const uint8_t* const bytecodeBase = ctx->bytecodeBase;
    // If you just joined the stream: ip is short for instruction pointer chat
    // The ip is mutable, so we need to use VM_SYNC_IP and VM_RELOAD_IP every time an opcode handler may access it or write to it
    uint32_t ip = ctx->ip;

    // Some opcodes have their handler or parts of their handler inlined
    // Those are opcodes that during real gameplay (using "--profile-opcodes") shown that, with inlining and keeping only the frequently called handle parts, we could squeeze MORE performance from the interpreter!
    while (codeEnd > ip) {
#ifdef ENABLE_VM_GML_PROFILER
        if (ctx->profiler != nullptr)
            Profiler_tickInstruction(ctx->profiler);
#endif
        uint32_t instrAddr = ip;
        uint32_t instr = BinaryUtils_readUint32Aligned(bytecodeBase + ip);
        ip += 4;

        // extraData pointer (may not be used depending on opcode)
        const uint8_t* extraData = bytecodeBase + ip;

        // If instruction has extra data (bit 30 set), advance IP past it
        if (instrHasExtraData(instr)) {
            ip += extraDataSize(instrType1(instr));
        }

        uint8_t opcode = instrOpcode(instr);

#ifdef ENABLE_VM_OPCODE_PROFILER
        if (ctx->opcodeProfilerEnabled) {
            ctx->opcodeCounts[opcode]++;
            ctx->opcodeVariantCounts[opcode * 256 + instrType1(instr) * 16 + instrType2(instr)]++;
            if (opcode == OP_BREAK) {
                int16_t breakType = instrInstanceType(instr);
                int idx = -breakType;
                if (idx >= 0 && 64 > idx) {
                    ctx->breakSubOpCounts[idx]++;
                }
            }
            // Capture actual runtime RValue types for arithmetic/comparison/conversion ops.
            // typeB = 0xF sentinel for unary ops (no second operand).
            uint8_t rvTypeA = 0xFF, rvTypeB = 0xF;
            switch (opcode) {
                case OP_MUL: case OP_DIV: case OP_REM: case OP_MOD:
                case OP_ADD: case OP_SUB: case OP_AND: case OP_OR:
                case OP_XOR: case OP_SHL: case OP_SHR: case OP_CMP:
                    if (ctx->stack.top >= 2) {
                        rvTypeA = ctx->stack.slots[ctx->stack.top - 2].type;
                        rvTypeB = ctx->stack.slots[ctx->stack.top - 1].type;
                    }
                    break;
                case OP_NEG: case OP_NOT: case OP_CONV:
                    if (ctx->stack.top >= 1) {
                        rvTypeA = ctx->stack.slots[ctx->stack.top - 1].type;
                    }
                    break;
            }
            if (rvTypeA != 0xFF) {
                ctx->opcodeRValueTypeCounts[opcode * 256 + (rvTypeA & 0xF) * 16 + (rvTypeB & 0xF)]++;
            }
        }
#endif

#ifdef ENABLE_VM_TRACING
        if (shlen(ctx->opcodesToBeTraced) > 0 && ctx->runner->frameCount >= ctx->traceBytecodeAfterFrame) {
            if (shgeti(ctx->opcodesToBeTraced, "*") != -1 || shgeti(ctx->opcodesToBeTraced, ctx->currentCodeName) != -1) {
                char opcodeStr[32], operandStr[256] = "", commentStr[128] = "";
                formatInstruction(ctx, ctx->bytecodeBase, instrAddr, instr, extraData, opcodeStr, sizeof(opcodeStr), operandStr, sizeof(operandStr), commentStr, sizeof(commentStr));

                char* stackBuf = formatStackContents(ctx);

                if (operandStr[0] != '\0') {
                    fprintf(stderr, "VM: [%s] @%04X [0x%08X] %s %s [stack=%d] %s\n", ctx->currentCodeName, instrAddr, instr, opcodeStr, operandStr, ctx->stack.top, stackBuf);
                } else {
                    fprintf(stderr, "VM: [%s] @%04X [0x%08X] %s [stack=%d] %s\n", ctx->currentCodeName, instrAddr, instr, opcodeStr, ctx->stack.top, stackBuf);
                }
                free(stackBuf);
            }
        }
#endif

        switch (opcode) {
            // Push instructions
            case OP_PUSH: {
                uint8_t type1 = instrType1(instr);
                // Inline fast paths for variable reads (not ints, doubles, etc, only VARIABLES) that are "normal" type (not arrays, not stacktop, and not the new fangled BC17 array reads)
                if (type1 == GML_TYPE_VARIABLE) {
                    uint32_t varRef = resolveVarOperand(extraData);
                    uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
                    if (varType == VARTYPE_NORMAL) {
                        Variable* varDef = resolveVarDef(ctx, varRef);
                        if (varDef->varID >= 0) {
                            int32_t instanceType = (int32_t) instrInstanceType(instr);
                            RValue val;
                            if (tryFastVarRead(ctx, instanceType, varDef, &val)) {
                                stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
                                break;
                            }
                        }
                    }
                }
                handlePush(ctx, instr, extraData, type1);
                break;
            }
            case OP_PUSHLOC: {
                uint32_t varRef = resolveVarOperand(extraData);
#if IS_BC17_OR_HIGHER_ENABLED
                uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
                if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
                    Variable* varDef = resolveVarDef(ctx, varRef);
                    uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                    require(ctx->localVarCount > localSlot);
                    pushTopLevelArrayRef(ctx, &ctx->localVars[localSlot]);
                    break;
                }
#endif
                // Locals are always non-builtin (varID >= 0); inline the read straight from localVars[].
                Variable* varDef = resolveVarDef(ctx, varRef);
                uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                require(ctx->localVarCount > localSlot);
                RValue val = ctx->localVars[localSlot];
                val.ownsReference = false;
                stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
                break;
            }
            case OP_PUSHGLB: {
                uint32_t varRef = resolveVarOperand(extraData);
#if IS_BC17_OR_HIGHER_ENABLED
                uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
                if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
                    Variable* varDef = resolveVarDef(ctx, varRef);
                    require(ctx->globalVarCount > (uint32_t) varDef->varID);
                    pushTopLevelArrayRef(ctx, &ctx->globalVars[varDef->varID]);
                    break;
                }
#endif
                // Globals are always non-builtin (varID >= 0); inline the read straight from globalVars[].
                Variable* varDef = resolveVarDef(ctx, varRef);
                require(ctx->globalVarCount > (uint32_t) varDef->varID);
                RValue val = ctx->globalVars[varDef->varID];
                val.ownsReference = false;
                stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
                break;
            }
            case OP_PUSHBLTN:
                handlePushBltn(ctx, instr, extraData);
                break;
            case OP_PUSHI:
                handlePushI(ctx, instr);
                break;

            // Pop instructions
            case OP_POP: {
                uint8_t type1 = instrType1(instr);
                uint32_t varRef = resolveVarOperand(extraData);
                uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
                int32_t instanceType = instrInstanceType(instr);
                if (type1 == GML_TYPE_VARIABLE && varType == VARTYPE_NORMAL) {
                    // Inline fast path for the simple variable-assignment case: type1==VARIABLE, which is ~99.998% of all Pops in real workloads
                    RValue val = stackPop(ctx);
                    resolveVariableWrite(ctx, instanceType, varRef, val);
                } else {
                    handlePop(ctx, instr, type1, varRef, varType, instanceType);
                }
                break;
            }
            case OP_POPZ:
                handlePopz(ctx);
                break;

            // Arithmetic
            // We keep the number + number operations inlined in executeLoop, keeping only the slow path for string concat/repetition
            case OP_ADD: {
                RValue* slotA = &ctx->stack.slots[ctx->stack.top - 2];
                RValue* slotB = &ctx->stack.slots[ctx->stack.top - 1];
                uint8_t aType = slotA->type;
                uint8_t bType = slotB->type;
                if ((aType == RVALUE_INT32 || aType == RVALUE_REAL) && (bType == RVALUE_INT32 || bType == RVALUE_REAL)) {
                    if (aType == RVALUE_INT32 && bType == RVALUE_INT32) {
                        slotA->int32 = slotA->int32 + slotB->int32;
                    } else {
                        // Read both operands as locals before writing back, since the union means
                        // slotA->real and slotA->int32 share storage.
                        GMLReal aVal = (aType == RVALUE_INT32) ? (GMLReal) slotA->int32 : slotA->real;
                        GMLReal bVal = (bType == RVALUE_INT32) ? (GMLReal) slotB->int32 : slotB->real;
                        slotA->real = aVal + bVal;
                        slotA->type = RVALUE_REAL;
                    }
#if IS_BC17_OR_HIGHER_ENABLED
                    if (IS_BC17_OR_HIGHER(ctx)) slotA->gmlStackType = instrType2(instr);
#endif
                    ctx->stack.top--;
                } else {
                    uint8_t resultType = instrType2(instr);
                    RValue b = stackPop(ctx);
                    RValue a = stackPop(ctx);
                    if (a.type == RVALUE_STRING || b.type == RVALUE_STRING) {
                        handleAddString(ctx, a, b, resultType);
                        break;
                    }
#ifndef NO_RVALUE_INT64
                    if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
                        stackPushTyped(ctx, RValue_makeInt64(a.int64 + b.int64), resultType);
                        break;
                    }
#endif
                    GMLReal result = RValue_toReal(a) + RValue_toReal(b);
                    RValue_free(&a);
                    RValue_free(&b);
                    stackPushTyped(ctx, RValue_makeReal(result), resultType);
                }
                break;
            }
            case OP_SUB: {
                RValue* slotA = &ctx->stack.slots[ctx->stack.top - 2];
                RValue* slotB = &ctx->stack.slots[ctx->stack.top - 1];
                uint8_t aType = slotA->type;
                uint8_t bType = slotB->type;
                if ((aType == RVALUE_INT32 || aType == RVALUE_REAL) && (bType == RVALUE_INT32 || bType == RVALUE_REAL)) {
                    if (aType == RVALUE_INT32 && bType == RVALUE_INT32) {
                        slotA->int32 = slotA->int32 - slotB->int32;
                    } else {
                        GMLReal aVal = (aType == RVALUE_INT32) ? (GMLReal) slotA->int32 : slotA->real;
                        GMLReal bVal = (bType == RVALUE_INT32) ? (GMLReal) slotB->int32 : slotB->real;
                        slotA->real = aVal - bVal;
                        slotA->type = RVALUE_REAL;
                    }
#if IS_BC17_OR_HIGHER_ENABLED
                    if (IS_BC17_OR_HIGHER(ctx)) slotA->gmlStackType = instrType2(instr);
#endif
                    ctx->stack.top--;
                } else {
                    uint8_t resultType = instrType2(instr);
                    RValue b = stackPop(ctx);
                    RValue a = stackPop(ctx);
#ifndef NO_RVALUE_INT64
                    if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
                        stackPushTyped(ctx, RValue_makeInt64(a.int64 - b.int64), resultType);
                        break;
                    }
#endif
                    GMLReal result = RValue_toReal(a) - RValue_toReal(b);
                    RValue_free(&a);
                    RValue_free(&b);
                    stackPushTyped(ctx, RValue_makeReal(result), resultType);
                }
                break;
            }
            case OP_MUL: {
                RValue* slotA = &ctx->stack.slots[ctx->stack.top - 2];
                RValue* slotB = &ctx->stack.slots[ctx->stack.top - 1];
                uint8_t aType = slotA->type;
                uint8_t bType = slotB->type;
                if ((aType == RVALUE_INT32 || aType == RVALUE_REAL) && (bType == RVALUE_INT32 || bType == RVALUE_REAL)) {
                    if (aType == RVALUE_INT32 && bType == RVALUE_INT32) {
                        slotA->int32 = slotA->int32 * slotB->int32;
                    } else {
                        GMLReal aVal = (aType == RVALUE_INT32) ? (GMLReal) slotA->int32 : slotA->real;
                        GMLReal bVal = (bType == RVALUE_INT32) ? (GMLReal) slotB->int32 : slotB->real;
                        slotA->real = aVal * bVal;
                        slotA->type = RVALUE_REAL;
                    }
#if IS_BC17_OR_HIGHER_ENABLED
                    if (IS_BC17_OR_HIGHER(ctx)) slotA->gmlStackType = instrType2(instr);
#endif
                    ctx->stack.top--;
                } else {
                    uint8_t resultType = instrType2(instr);
                    RValue b = stackPop(ctx);
                    RValue a = stackPop(ctx);
                    if (a.type == RVALUE_STRING) {
                        handleMulString(ctx, a, b, resultType);
                        break;
                    }
#ifndef NO_RVALUE_INT64
                    if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
                        stackPushTyped(ctx, RValue_makeInt64(a.int64 * b.int64), resultType);
                        break;
                    }
#endif
                    GMLReal result = RValue_toReal(a) * RValue_toReal(b);
                    RValue_free(&a);
                    RValue_free(&b);
                    stackPushTyped(ctx, RValue_makeReal(result), resultType);
                }
                break;
            }
            case OP_DIV: handleDiv(ctx, instr); break;
            case OP_REM: handleRem(ctx, instr); break;
            case OP_MOD: handleMod(ctx, instr); break;

            // Bitwise / Logical
            case OP_AND: handleAnd(ctx, instr); break;
            case OP_OR:  handleOr(ctx, instr);  break;
            case OP_XOR: handleXor(ctx, instr); break;
            case OP_SHL: handleShl(ctx, instr); break;
            case OP_SHR: handleShr(ctx, instr); break;

            // Unary
            case OP_NEG: handleNeg(ctx, instr); break;
            case OP_NOT: handleNot(ctx, instr); break;

            // Type conversion
            case OP_CONV: {
                uint8_t srcType = instrType1(instr);
                uint8_t dstType = instrType2(instr);
                uint8_t convKey = (uint8_t) ((dstType << 4) | srcType);
                RValue* top = &ctx->stack.slots[ctx->stack.top - 1];
                bool fastHit = false;

                // Inline fast paths for the four conversions that account for ~93% of all Conv opcodes in real workloads
                switch (convKey) {
                    case 0x52: // Int32 -> Variable (pure passthrough; just retag stack slot)
                        fastHit = true;
                        break;
                    case 0x45: // Variable -> Bool
                        if (top->type == RVALUE_INT32) {
                            top->int32 = top->int32 > 0 ? 1 : 0;
                            top->type = RVALUE_BOOL;
                            fastHit = true;
                        } else if (top->type == RVALUE_BOOL) {
                            // Already 0/1; nothing to do
                            fastHit = true;
                        } else if (top->type == RVALUE_REAL) {
                            top->int32 = top->real > (GMLReal) 0.5 ? 1 : 0;
                            top->type = RVALUE_BOOL;
                            fastHit = true;
                        }
                        break;
                    case 0x25: // Variable -> Int32
                        if (top->type == RVALUE_INT32) {
                            fastHit = true;
                        } else if (top->type == RVALUE_BOOL) {
                            top->type = RVALUE_INT32;
                            fastHit = true;
                        } else if (top->type == RVALUE_REAL) {
                            top->int32 = (int32_t) top->real;
                            top->type = RVALUE_INT32;
                            fastHit = true;
                        }
                        break;
                    case 0x02: // Int32 -> Double (Real)
                        top->real = (GMLReal) top->int32;
                        top->type = RVALUE_REAL;
                        fastHit = true;
                        break;
                }

                if (fastHit) {
#if IS_BC17_OR_HIGHER_ENABLED
                    if (IS_BC17_OR_HIGHER(ctx)) top->gmlStackType = dstType;
#endif
                } else {
                    handleConv(ctx, srcType, dstType, convKey);
                }
                break;
            }

            // Comparison
            case OP_CMP: {
                RValue* slotA = &ctx->stack.slots[ctx->stack.top - 2];
                RValue* slotB = &ctx->stack.slots[ctx->stack.top - 1];
                uint8_t typeA = slotA->type;
                uint8_t typeB = slotB->type;

                // Inline numeric fast path
                bool aNumeric = (typeA == RVALUE_INT32 || typeA == RVALUE_REAL);
                bool bNumeric = (typeB == RVALUE_INT32 || typeB == RVALUE_REAL);
                if (aNumeric && bNumeric) {
                    bool result;
                    if (typeA == RVALUE_INT32 && typeB == RVALUE_INT32) {
                        int32_t a = slotA->int32;
                        int32_t b = slotB->int32;
                        switch (instrCmpKind(instr)) {
                            case CMP_LT:  result = b > a;  break;
                            case CMP_LTE: result = b >= a; break;
                            case CMP_EQ:  result = a == b; break;
                            case CMP_NEQ: result = a != b; break;
                            case CMP_GTE: result = a >= b; break;
                            case CMP_GT:  result = a > b;  break;
                            default:      result = false;  break;
                        }
                    } else {
                        GMLReal a = (typeA == RVALUE_REAL) ? slotA->real : (GMLReal) slotA->int32;
                        GMLReal b = (typeB == RVALUE_REAL) ? slotB->real : (GMLReal) slotB->int32;
                        switch (instrCmpKind(instr)) {
                            case CMP_LT:  result = b > a;  break;
                            case CMP_LTE: result = b >= a; break;
                            case CMP_EQ:  result = a == b; break;
                            case CMP_NEQ: result = a != b; break;
                            case CMP_GTE: result = a >= b; break;
                            case CMP_GT:  result = a > b;  break;
                            default:      result = false;  break;
                        }
                    }
                    slotA->int32 = result ? 1 : 0;
                    slotA->type = RVALUE_BOOL;
#if IS_BC17_OR_HIGHER_ENABLED
                    if (IS_BC17_OR_HIGHER(ctx)) slotA->gmlStackType = GML_TYPE_BOOL;
#endif
                    ctx->stack.top--;
                } else {
                    handleCmp(ctx, instr);
                }
                break;
            }

            // Duplicate
            case OP_DUP:
                handleDup(ctx, instr);
                break;

            // Branches
            // The reason why these (the branches opcodes) are inlined is because they access ctx->ip
            // So, because they are short n' sweet, we prefer to keep them inlined to avoid any reloading shenanigans that the compiler may do
            case OP_B: {
                int32_t offset = instrJumpOffset(instr);
                ip = instrAddr + offset;
                break;
            }
            case OP_BT: {
                bool condition = stackPopInt32(ctx) != 0;
                if (condition == true) {
                    int32_t offset = instrJumpOffset(instr);
                    ip = instrAddr + offset;
                }
                break;
            }
            case OP_BF: {
                bool condition = stackPopInt32(ctx) != 0;
                if (condition == false) {
                    int32_t offset = instrJumpOffset(instr);
                    ip = instrAddr + offset;
                }
                break;
            }

            // Function call
            case OP_CALL:
                VM_SYNC_IP();
                handleCall(ctx, instr, extraData);
                break;
#if IS_BC17_OR_HIGHER_ENABLED
            case OP_CALLV:
                VM_SYNC_IP();
                handleCallV(ctx, instr);
                break;
#endif

            // Return
            case OP_RET: {
                RValue retVal = stackPop(ctx);
                return retVal;
            }

            // Exit (no return value)
            case OP_EXIT:
                return RValue_makeUndefined();

            // Environment (with-statements)
            case OP_PUSHENV:
                VM_SYNC_IP();
                handlePushEnv(ctx, instr, instrAddr);
                VM_RELOAD_IP();
                break;
            case OP_POPENV:
                VM_SYNC_IP();
                handlePopEnv(ctx, instr, instrAddr);
                VM_RELOAD_IP();
                break;

            // Break (extended opcodes in V17+, no-op/debug in V16)
            case OP_BREAK:
#if IS_BC17_OR_HIGHER_ENABLED
                handleBreak(ctx, instr, instrAddr);
#endif
                break;

            default:
                fprintf(stderr, "VM: Unknown opcode 0x%02X at offset %u\n", opcode, instrAddr);
                abort();
        }
    }

    return RValue_makeUndefined();
}

// ===[ Public API ]===

VMContext* VM_create(DataWin* dataWin) {
#ifdef PLATFORM_PS2
    // Place VMContext in scratchpad RAM
    requireMessage(16384 >= sizeof(VMContext), "VMContext exceeds PS2 scratchpad size (16 KB)");
    VMContext* ctx = (VMContext*) 0x70000000;
    memset(ctx, 0, sizeof(VMContext));
#else
    VMContext* ctx = safeCalloc(1, sizeof(VMContext));
#endif
    ctx->dataWin = dataWin;
    ctx->stack.top = 0;
    ctx->selfId = -1;
    ctx->otherId = -1;
    ctx->callDepth = 0;
    ctx->currentEventType = -1;
    ctx->currentEventSubtype = -1;
    ctx->currentEventObjectIndex = -1;

    ctx->profiler = nullptr; // lazily allocated by Profiler_setEnabled(&ctx->profiler, true)

    // Validate that no code entry exceeds MAX_CODE_LOCALS (the VM uses stack-allocated arrays of this size)
    repeat(dataWin->code.count, i) {
        CodeEntry* entry = &dataWin->code.entries[i];
        requireMessageFormatted(MAX_CODE_LOCALS > entry->localsCount, "Code %s has too many locals!", entry->name);
    }

    VMBuiltins_checkIfBuiltinVarTableIsSorted();

    // Pre-resolve built-in variable IDs (replaces runtime strcmp chains with O(1) switch dispatch)
    repeat(dataWin->vari.variableCount, i) {
        Variable* var = &dataWin->vari.variables[i];
        // varID == -6 is the BC16 built-in sentinel.
        // In BC17, argument variables have instanceType == -6 (Builtin) with varID >= 0, so we also check instanceType.
        if (var->varID == -6 || var->instanceType == -6) {
            var->builtinVarId = VMBuiltins_resolveBuiltinVarId(var->name);
        } else {
            var->builtinVarId = BUILTIN_VAR_UNKNOWN;
        }
    }

    // Build reference lookup maps (file buffer stays read-only)
    patchReferenceOperands(ctx);

    // Scan VARI entries to find max varID for global scope
    // Built-in variables have varID == -6 (sentinel), skip those
    uint32_t maxGlobalVarID = 0;
    forEach(Variable, v, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (0 > v->varID) continue;
        if (v->instanceType == INSTANCE_GLOBAL) {
            if ((uint32_t) v->varID + 1 > maxGlobalVarID) maxGlobalVarID = (uint32_t) v->varID + 1;
        }
    }

    ctx->globalVarCount = maxGlobalVarID;
    ctx->globalVars = safeCalloc(maxGlobalVarID, sizeof(RValue));
    repeat(maxGlobalVarID, i) {
        ctx->globalVars[i].type = RVALUE_UNDEFINED;
    }

    ctx->currentCodeIndex = -1;

    // V17+ static initialization tracking
    if (dataWin->gen8.bytecodeVersion >= 17) {
        ctx->staticInitialized = safeCalloc(dataWin->code.count, sizeof(bool));
    } else {
        ctx->staticInitialized = nullptr;
    }
    ctx->currentArrayOwner = nullptr;
    ctx->savearefBalance = 0;

    // Find the varID for "creator" self variable (used by instance_create)
    ctx->creatorVarID = -1;
    forEach(Variable, cv, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (cv->instanceType == INSTANCE_SELF && cv->varID >= 0 && strcmp(cv->name, "creator") == 0) {
            ctx->creatorVarID = cv->varID;
            break;
        }
    }

    // Build globalVarNameMap: varName -> varID for global variables
    ctx->globalVarNameMap = nullptr;
    forEach(Variable, v2, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (v2->instanceType == INSTANCE_GLOBAL && v2->varID >= 0) {
            ptrdiff_t existing = shgeti(ctx->globalVarNameMap, (char*) v2->name);
            if (0 > existing) {
                shput(ctx->globalVarNameMap, (char*) v2->name, v2->varID);
            }
        }
    }

    // Build selfVarNameMap: varName -> varID for self/instance-scoped variables.
    ctx->selfVarNameMap = nullptr;
    forEach(Variable, v3, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (v3->varID >= 0 && (v3->instanceType == INSTANCE_SELF || 0 > v3->instanceType)) {
            ptrdiff_t existing = shgeti(ctx->selfVarNameMap, (char*) v3->name);
            if (0 > existing) {
                shput(ctx->selfVarNameMap, (char*) v3->name, v3->varID);
            }
        }
    }

    // Build funcName -> codeIndex hash map from SCPT chunk
    ctx->funcMap = nullptr;
    forEach(Script, s, dataWin->scpt.scripts, dataWin->scpt.count) {
        if (s->name != nullptr && s->codeId >= 0) {
            if (dataWin->code.count > (uint32_t) s->codeId) {
                const char* codeName = dataWin->code.entries[s->codeId].name;
                // Map the full code entry name (e.g. "gml_Script_SCR_GAMESTART")
                shput(ctx->funcMap, (char*) codeName, s->codeId);
                // Also map the bare script name (e.g. "SCR_GAMESTART")
                // since the FUNC chunk references use bare names in CALL instructions
                shput(ctx->funcMap, (char*) s->name, s->codeId);
            }
        }
    }

    // Also map code entry names directly for non-script code (object events, room creation codes, etc.)
    repeat(dataWin->code.count, i) {
        const char* codeName = dataWin->code.entries[i].name;
        ptrdiff_t existing = shgeti(ctx->funcMap, (char*) codeName);
        if (0 > existing) {
            shput(ctx->funcMap, (char*) codeName, (int32_t) i);
        }
    }

    // Build codeName -> CodeLocals* hash map
    ctx->codeLocalsMap = nullptr;
    repeat(dataWin->func.codeLocalsCount, i) {
        CodeLocals* cl = &dataWin->func.codeLocals[i];
        shput(ctx->codeLocalsMap, safeStrdup(cl->name), cl);
        // In bytecode 17+, CodeLocals uses "gml_GlobalScript_" prefix but callable CODE entries use "gml_Script_", so we'll map the "gml_Script_" variant too
        if (dataWin->gen8.bytecodeVersion >= 17) {
            if (strncmp(cl->name, "gml_GlobalScript_", 17) == 0) {
                char scriptName[512];
                snprintf(scriptName, sizeof(scriptName), "gml_Script_%s", cl->name + 17);
                shput(ctx->codeLocalsMap, safeStrdup(scriptName), cl);
            }
        }
    }

    // BC17+: build per-CodeLocals varID -> slot hmap so resolveLocalSlot is O(1)
    // We NEED to do it with the "code.count" because YoYo Games in their infinite wisdom thought "what if... we just didn't include some local variables in the localVars map? heck, sometimes we can just NOT include any CodeLocals!"... fun!
    ctx->codeLocalsSlotMaps = nullptr;
    if (dataWin->gen8.bytecodeVersion >= 17) {
        ctx->codeLocalsSlotMaps = safeCalloc(dataWin->code.count, sizeof(*ctx->codeLocalsSlotMaps));
    }

    // Register built-in functions
    VMBuiltins_registerAll(ctx);

    // Pre-resolve all FUNC entries to cached builtin pointers or script code indices.
    // This eliminates per-call string hash lookups in handleCall.
    ctx->funcCallCacheCount = dataWin->func.functionCount;
    ctx->funcCallCache = safeMalloc(dataWin->func.functionCount * sizeof(FuncCallCache));
    repeat(dataWin->func.functionCount, i) {
        const char* name = dataWin->func.functions[i].name;
        BuiltinFunc builtin = VM_findBuiltin(ctx, name);
        ctx->funcCallCache[i].builtin = (void*) builtin;
        if (builtin != nullptr) {
            ctx->funcCallCache[i].scriptCodeIndex = -1;
        } else {
            ptrdiff_t mapIdx = shgeti(ctx->funcMap, (char*) name);
            ctx->funcCallCache[i].scriptCodeIndex = (mapIdx >= 0) ? ctx->funcMap[mapIdx].value : -1;
        }
    }

    fprintf(stderr, "VM: Initialized with %u global vars, sparse self vars (hashmap), %u functions mapped\n", ctx->globalVarCount, (uint32_t) shlen(ctx->funcMap));

    return ctx;
}

void VM_reset(VMContext* ctx) {
    // Reset all global variables to undefined
    repeat(ctx->globalVarCount, i) {
        RValue_free(&ctx->globalVars[i]);
        ctx->globalVars[i].type = RVALUE_UNDEFINED;
    }

    // Reset stack
    ctx->stack.top = 0;

    // Free any remaining call frames
    CallFrame* frame = ctx->callStack;
    while (frame != nullptr) {
        CallFrame* parent = frame->parent;
        free(frame);
        frame = parent;
    }
    ctx->callStack = nullptr;
    ctx->callDepth = 0;

    // Free any remaining env frames
    EnvFrame* envFrame = ctx->envStack;
    while (envFrame != nullptr) {
        EnvFrame* parent = envFrame->parent;
        arrfree(envFrame->instanceList);
        free(envFrame);
        envFrame = parent;
    }
    ctx->envStack = nullptr;

    // Reset execution state
    ctx->currentInstance = nullptr;
    ctx->otherInstance = nullptr;
    ctx->selfId = -1;
    ctx->otherId = -1;
    ctx->currentEventType = -1;
    ctx->currentEventSubtype = -1;
    ctx->currentEventObjectIndex = -1;
    ctx->scriptArgs = nullptr;
    ctx->scriptArgCount = 0;
    ctx->currentCodeName = nullptr;
    ctx->localVars = nullptr;
    ctx->localVarCount = 0;
    ctx->currentCodeLocalsSlotMap = nullptr;
    ctx->actionRelativeFlag = false;

    fprintf(stderr, "VM: Reset complete (%u global vars cleared)\n", ctx->globalVarCount);
}

static CodeLocals* resolveCodeLocals(VMContext* ctx, const char* codeName) {
    return shget(ctx->codeLocalsMap, (char*) codeName);
}

// Sets the currentCodeLocalsSlotMap for BC17+ games
static void setCurrentCodeLocalsSlotMap(VMContext* ctx) {
    if (IS_BC17_OR_HIGHER(ctx)) {
        ctx->currentCodeLocalsSlotMap = &ctx->codeLocalsSlotMaps[ctx->currentCodeIndex];
    }
}

static uint32_t computeLocalsCount(VMContext* ctx, CodeEntry* code) {
    if (IS_BC16_OR_BELOW(ctx)) {
        return code->localsCount;
    } else {
        // We can't trust localVarCount in GM:S 2.3+, so we will get our cached map
        // It is NOT the "right" localsCount because it may increase during runtime, but for now, this shall do
        return IntIntHashMap_count(&ctx->codeLocalsSlotMaps[ctx->currentCodeIndex]);
    }
}

RValue VM_executeCode(VMContext* ctx, int32_t codeIndex) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    ctx->bytecodeBase = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);
    ctx->ip = code->offset;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;
    ctx->currentCodeIndex = codeIndex;

    setCurrentCodeLocalsSlotMap(ctx);

    uint32_t localsCount = computeLocalsCount(ctx, code);
    RValue localVars[MAX_CODE_LOCALS];
    ctx->localVars = localVars;
    ctx->localVarCount = localsCount;
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Reset stack for top-level execution
    ctx->stack.top = 0;

    int32_t savedSavearefBalance = ctx->savearefBalance;
    ctx->savearefBalance = 0;

#ifdef ENABLE_VM_GML_PROFILER
    Profiler_enter(ctx->profiler, code->name);
#endif
    RValue result = executeLoop(ctx);
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_exit(ctx->profiler);
#endif

    requireMessage(ctx->savearefBalance == 0, "SAVEAREF/RESTOREAREF imbalance at end of VM_executeCode (unpaired SAVEAREF)");
    ctx->savearefBalance = savedSavearefBalance;

    // Free locals (decRefs owned arrays, frees owned strings)
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }
    ctx->localVars = nullptr;
    ctx->localVarCount = 0;

    return result;
}


RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    // Save current frame
    CallFrame frame = (CallFrame) {
        .savedIP = ctx->ip,
        .savedCodeEnd = ctx->codeEnd,
        .savedBytecodeBase = ctx->bytecodeBase,
        .savedLocals = ctx->localVars,
        .savedLocalsCount = ctx->localVarCount,
        .savedCodeName = ctx->currentCodeName,
        .savedSavearefBalance = ctx->savearefBalance,
        .savedCodeLocalsSlotMap = ctx->currentCodeLocalsSlotMap,
        .savedScriptArgs = ctx->scriptArgs,
        .savedScriptArgCount = ctx->scriptArgCount,
        .savedCurrentCodeIndex = ctx->currentCodeIndex,
        .parent = ctx->callStack,
    };
    ctx->callStack = &frame;
    ctx->callDepth++;

    // Set up callee
    ctx->bytecodeBase = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);
    ctx->ip = code->offset;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;
    ctx->currentCodeIndex = codeIndex;

    setCurrentCodeLocalsSlotMap(ctx);

    uint32_t localsCount = computeLocalsCount(ctx, code);
    // We use fixed-size arrays instead of VLAs because it seems that using multiple VLAs in a single function things get corrupted somehow?
    // So when you see this MAX_CODE_LOCALS and GML_MAX_ARGUMENTS, you can shake your fist in the air and say "damn you MIPS!!1"
    RValue localVars[MAX_CODE_LOCALS];
    ctx->localVars = localVars;
    ctx->localVarCount = localsCount;
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Store arguments in scriptArgs (mirrors GMS 1.4's global argument stack).
    // Callee takes an INDEPENDENT reference for strings (strdup) and arrays (incRef) so
    // the caller's original args remain valid and owner-tracked by the caller.
    RValue scriptArgs[GML_MAX_ARGUMENTS];
    ctx->scriptArgs = scriptArgs;
    ctx->scriptArgCount = argCount;
    if (argCount > 0 && args != nullptr) {
        repeat(argCount, argIdx) {
            RValue argCopy = args[argIdx];
            if (argCopy.type == RVALUE_STRING && argCopy.ownsReference && argCopy.string != nullptr) {
                argCopy.string = safeStrdup(argCopy.string);
            } else if (argCopy.type == RVALUE_ARRAY && argCopy.array != nullptr) {
                GMLArray_incRef(argCopy.array);
                argCopy.ownsReference = true;
#if IS_BC17_OR_HIGHER_ENABLED
            } else if (argCopy.type == RVALUE_METHOD && argCopy.method != nullptr) {
                GMLMethod_incRef(argCopy.method);
                argCopy.ownsReference = true;
#endif
            } else if (argCopy.type == RVALUE_STRUCT && argCopy.structInst != nullptr) {
                Instance_structIncRef(argCopy.structInst);
                argCopy.ownsReference = true;
            }
            ctx->scriptArgs[argIdx] = argCopy;
        }
    }

    ctx->savearefBalance = 0;

    // Execute the callee
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_enter(ctx->profiler, code->name);
#endif
    RValue result = executeLoop(ctx);
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_exit(ctx->profiler);
#endif

    requireMessage(ctx->savearefBalance == 0, "SAVEAREF/RESTOREAREF imbalance at end of VM_callCodeIndex (unpaired SAVEAREF)");

    // Strengthen result BEFORE freeing callee locals/scriptArgs: if result is a weak view into callee state, the upcoming frees would leave a dangling pointer.
    // For owning results, the refCount/string buffer stays valid (the callee transferred one ownership slot to us).
    if (result.type == RVALUE_STRING && !result.ownsReference && result.string != nullptr) {
        result = RValue_makeOwnedString(safeStrdup(result.string));
    } else if (result.type == RVALUE_ARRAY && !result.ownsReference && result.array != nullptr) {
        GMLArray_incRef(result.array);
        result.ownsReference = true;
#if IS_BC17_OR_HIGHER_ENABLED
    } else if (result.type == RVALUE_METHOD && !result.ownsReference && result.method != nullptr) {
        GMLMethod_incRef(result.method);
        result.ownsReference = true;
#endif
    } else if (result.type == RVALUE_STRUCT && !result.ownsReference && result.structInst != nullptr) {
        Instance_structIncRef(result.structInst);
        result.ownsReference = true;
    }

    // Restore caller frame
    CallFrame* saved = ctx->callStack;
    ctx->ip = saved->savedIP;
    ctx->codeEnd = saved->savedCodeEnd;
    ctx->bytecodeBase = saved->savedBytecodeBase;

    // Free callee locals
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }

    // Free callee script args
    repeat(ctx->scriptArgCount, i) {
        RValue_free(&ctx->scriptArgs[i]);
    }

    ctx->localVars = saved->savedLocals;
    ctx->localVarCount = saved->savedLocalsCount;
    ctx->currentCodeLocalsSlotMap = saved->savedCodeLocalsSlotMap;
    ctx->scriptArgs = saved->savedScriptArgs;
    ctx->scriptArgCount = saved->savedScriptArgCount;
    ctx->currentCodeName = saved->savedCodeName;
    ctx->currentCodeIndex = saved->savedCurrentCodeIndex;
    ctx->savearefBalance = saved->savedSavearefBalance;
    ctx->callStack = saved->parent;
    ctx->callDepth--;

    return result;
}

// ===[ Disassembler ]===

static char gmlTypeChar(uint8_t type) {
    switch (type) {
        case GML_TYPE_DOUBLE:   return 'd';
        case GML_TYPE_FLOAT:    return 'f';
        case GML_TYPE_INT32:    return 'i';
        case GML_TYPE_INT64:    return 'l';
        case GML_TYPE_BOOL:     return 'b';
        case GML_TYPE_VARIABLE: return 'v';
        case GML_TYPE_STRING:   return 's';
        case GML_TYPE_INT16:    return 'e';
        default:                return '?';
    }
}

static const char* cmpKindName(uint8_t kind) {
    switch (kind) {
        case CMP_LT:  return "LT";
        case CMP_LTE: return "LTE";
        case CMP_EQ:  return "EQ";
        case CMP_NEQ: return "NEQ";
        case CMP_GTE: return "GTE";
        case CMP_GT:  return "GT";
        default:      return "???";
    }
}

static const char* varTypeName(uint32_t varRef) {
    uint8_t varType = (varRef >> 24) & 0xF8;
    switch (varType) {
        case VARTYPE_ARRAY:    return "Array";
        case VARTYPE_STACKTOP: return "StackTop";
        case VARTYPE_NORMAL:   return "Normal";
        case VARTYPE_INSTANCE: return "Instance";
        default:               return "Unknown";
    }
}

static const char* disasmScopeName(VMContext* ctx, int32_t instanceType) {
    switch (instanceType) {
        case INSTANCE_SELF:      return "self";
        case INSTANCE_OTHER:     return "other";
        case INSTANCE_ALL:       return "all";
        case INSTANCE_NOONE:     return "noone";
        case INSTANCE_GLOBAL:    return "global";
        case INSTANCE_LOCAL:     return "local";
        case INSTANCE_STACKTOP:  return "stacktop";
        default:
            if (instanceType >= 0 && ctx->dataWin->objt.count > (uint32_t) instanceType) {
                return ctx->dataWin->objt.objects[instanceType].name;
            }
            return "unknown";
    }
}

// Formats a variable operand for disassembly: "scope.varName [varType]"
// If scopeOverride is set (e.g. "local", "global"), uses that instead of resolving instrInstType.
// Shows VARI instanceType mismatch annotation when scopeOverride is nullptr and types differ.
static void disasmFormatVar(VMContext* ctx, const uint8_t* extraData, const char* scopeOverride, int32_t instrInstType, char* buf, size_t bufSize) {
    uint32_t varRef = resolveVarOperand(extraData);
    Variable* varDef = resolveVarDef(ctx, varRef);
    const char* vType = varTypeName(varRef);

    // For StackTop and Array variable types, the actual instance type comes from the stack at runtime, not from the instruction operand.
    // Use the VARI entry's instanceType instead, since the instruction's instanceType is meaningless for these access types.
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (varType == VARTYPE_STACKTOP || varType == VARTYPE_ARRAY) {
        const char* scope = scopeOverride != nullptr ? scopeOverride : disasmScopeName(ctx, varDef->instanceType);
        snprintf(buf, bufSize, "%s.%s [%s]", scope, varDef->name, vType);
        return;
    }

    const char* scope = scopeOverride != nullptr ? scopeOverride : disasmScopeName(ctx, instrInstType);

    if (scopeOverride == nullptr && varDef->instanceType != instrInstType) {
        const char* variScope = disasmScopeName(ctx, varDef->instanceType);
        snprintf(buf, bufSize, "%s.%s [%s] (VARI: %s, instr: %s)", scope, varDef->name, vType, variScope, scope);
    } else {
        snprintf(buf, bufSize, "%s.%s [%s]", scope, varDef->name, vType);
    }
}

// Returns stack effect comment for a variable access instruction
static void disasmFormatVarComment(VMContext* ctx, const uint8_t* extraData, bool isPop, char* buf, size_t bufSize) {
    uint32_t varRef = resolveVarOperand(extraData);
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (isPop) {
        switch (varType) {
            case VARTYPE_ARRAY:    snprintf(buf, bufSize, "// pops: [arrayIndex, instanceType, value]"); break;
            case VARTYPE_STACKTOP: snprintf(buf, bufSize, "// pops: [instanceType, value]"); break;
            default:               snprintf(buf, bufSize, "// pops: [value]"); break;
        }
    } else {
        switch (varType) {
            case VARTYPE_ARRAY:    snprintf(buf, bufSize, "// pops: [arrayIndex, instanceType] -> pushes: [value]"); break;
            case VARTYPE_STACKTOP: snprintf(buf, bufSize, "// pops: [instanceType] -> pushes: [value]"); break;
            default:               snprintf(buf, bufSize, "// pushes: [value]"); break;
        }
    }
}

// Formats a single instruction into opcodeStr, operandStr, and commentStr buffers.
// Used by both VM_disassemble and --trace-opcodes.
// bytecodeBase is needed because the disassembler and trace have it from different sources.
static void formatInstruction(VMContext* ctx, const uint8_t* bytecodeBase, uint32_t instrAddr, uint32_t instr, const uint8_t* extraData,
                              char* opcodeStr, size_t opcodeSize, char* operandStr, size_t operandSize, char* commentStr, size_t commentSize) {
    DataWin* dw = ctx->dataWin;
    uint8_t opcode = instrOpcode(instr);
    uint8_t type1 = instrType1(instr);
    uint8_t type2 = instrType2(instr);
    int16_t instType = instrInstanceType(instr);

    switch (opcode) {
        // Binary arithmetic/logic
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_REM: case OP_MOD: case OP_AND: case OP_OR:
        case OP_XOR: case OP_SHL: case OP_SHR:
            snprintf(opcodeStr, opcodeSize, "%s.%c.%c", opcodeName(opcode), gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(commentStr, commentSize, "// pops: [a, b] -> pushes: [result]");
            break;

        // Unary
        case OP_NEG:
            snprintf(opcodeStr, opcodeSize, "Neg.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [result]");
            break;
        case OP_NOT:
            snprintf(opcodeStr, opcodeSize, "Not.%c", gmlTypeChar(type1));
            if (type1 == GML_TYPE_BOOL) {
                snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [bool] (logical NOT)");
            } else {
                snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [int] (bitwise NOT)");
            }
            break;

        // Type conversion
        case OP_CONV:
            snprintf(opcodeStr, opcodeSize, "Conv.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(commentStr, commentSize, "// pops: [%c] -> pushes: [%c]", gmlTypeChar(type2), gmlTypeChar(type1));
            break;

        // Comparison
        case OP_CMP:
            snprintf(opcodeStr, opcodeSize, "Cmp.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(operandStr, operandSize, "%s", cmpKindName(instrCmpKind(instr)));
            snprintf(commentStr, commentSize, "// pops: [a, b] -> pushes: [bool]");
            break;

        // Push
        case OP_PUSH: {
            switch (type1) {
                case GML_TYPE_DOUBLE:
                    snprintf(opcodeStr, opcodeSize, "Push.d");
                    snprintf(operandStr, operandSize, "%g", BinaryUtils_readFloat64(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [double]");
                    break;
                case GML_TYPE_FLOAT:
                    snprintf(opcodeStr, opcodeSize, "Push.f");
                    snprintf(operandStr, operandSize, "%g", (double) BinaryUtils_readFloat32(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [float]");
                    break;
                case GML_TYPE_INT32:
                    snprintf(opcodeStr, opcodeSize, "Push.i");
                    snprintf(operandStr, operandSize, "%d", BinaryUtils_readInt32(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [int32]");
                    break;
                case GML_TYPE_INT64:
                    snprintf(opcodeStr, opcodeSize, "Push.l");
                    snprintf(operandStr, operandSize, "%lld", (long long) BinaryUtils_readInt64(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [int64]");
                    break;
                case GML_TYPE_BOOL:
                    snprintf(opcodeStr, opcodeSize, "Push.b");
                    snprintf(operandStr, operandSize, "%s", BinaryUtils_readInt32(extraData) != 0 ? "true" : "false");
                    snprintf(commentStr, commentSize, "// pushes: [bool]");
                    break;
                case GML_TYPE_STRING: {
                    snprintf(opcodeStr, opcodeSize, "Push.s");
                    int32_t strIdx = BinaryUtils_readInt32(extraData);
                    if (strIdx >= 0 && dw->strg.count > (uint32_t) strIdx) {
                        const char* str = dw->strg.strings[strIdx];
                        if (strlen(str) > 60) {
                            snprintf(operandStr, operandSize, "\"%.57s...\"", str);
                        } else {
                            snprintf(operandStr, operandSize, "\"%s\"", str);
                        }
                    } else {
                        snprintf(operandStr, operandSize, "[string:%d]", strIdx);
                    }
                    snprintf(commentStr, commentSize, "// pushes: [string]");
                    break;
                }
                case GML_TYPE_VARIABLE:
                    snprintf(opcodeStr, opcodeSize, "Push.v");
                    disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
                    disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
                    break;
                case GML_TYPE_INT16:
                    snprintf(opcodeStr, opcodeSize, "Push.e");
                    snprintf(operandStr, operandSize, "%d", (int32_t) instType);
                    snprintf(commentStr, commentSize, "// pushes: [int16]");
                    break;
                default:
                    snprintf(opcodeStr, opcodeSize, "Push.?");
                    snprintf(operandStr, operandSize, "(unknown type 0x%X)", type1);
                    break;
            }
            break;
        }

        // Scoped pushes
        case OP_PUSHLOC:
            snprintf(opcodeStr, opcodeSize, "PushLoc.v");
            disasmFormatVar(ctx, extraData, "local", (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHGLB:
            snprintf(opcodeStr, opcodeSize, "PushGlb.v");
            disasmFormatVar(ctx, extraData, "global", (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHBLTN:
            snprintf(opcodeStr, opcodeSize, "PushBltn.v");
            disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;

        // PushI (int16 immediate)
        case OP_PUSHI:
            snprintf(opcodeStr, opcodeSize, "PushI.e");
            snprintf(operandStr, operandSize, "%d", (int32_t) instType);
            snprintf(commentStr, commentSize, "// pushes: [int16]");
            break;

        // Pop (store to variable)
        case OP_POP:
            snprintf(opcodeStr, opcodeSize, "Pop.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, true, commentStr, commentSize);
            break;

        // Unconditional branch
        case OP_B: {
            snprintf(opcodeStr, opcodeSize, "B");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            break;
        }

        // Conditional branches
        case OP_BT: {
            snprintf(opcodeStr, opcodeSize, "BT");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            snprintf(commentStr, commentSize, "// pops: [bool]");
            break;
        }
        case OP_BF: {
            snprintf(opcodeStr, opcodeSize, "BF");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            snprintf(commentStr, commentSize, "// pops: [bool]");
            break;
        }

        // With-statement: PushEnv
        case OP_PUSHENV: {
            snprintf(opcodeStr, opcodeSize, "PushEnv");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            // Peek at previous instruction to identify the target object
            const char* targetName = nullptr;
            if (instrAddr >= 4) {
                uint32_t prevInstr = BinaryUtils_readUint32(bytecodeBase + instrAddr - 4);
                if (instrOpcode(prevInstr) == OP_PUSHI) {
                    int16_t objIdx = (int16_t) (prevInstr & 0xFFFF);
                    targetName = disasmScopeName(ctx, (int32_t) objIdx);
                }
            }
            if (targetName != nullptr) {
                snprintf(operandStr, operandSize, "%s (target: L_%04X, offset: %+d)", targetName, target, offset);
            } else {
                snprintf(operandStr, operandSize, "(target: L_%04X, offset: %+d)", target, offset);
            }
            snprintf(commentStr, commentSize, "// pops: [target]");
            break;
        }

        // With-statement: PopEnv
        case OP_POPENV: {
            snprintf(opcodeStr, opcodeSize, "PopEnv");
            if ((instr & 0x00FFFFFF) == 0xF00000) {
                snprintf(operandStr, operandSize, "[exit]");
            } else {
                int32_t offset = instrJumpOffset(instr);
                uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                snprintf(operandStr, operandSize, "(target: L_%04X, offset: %+d)", target, offset);
            }
            break;
        }

        // Function call
        case OP_CALL: {
            snprintf(opcodeStr, opcodeSize, "Call.i");
            int32_t argCount = instr & 0xFFFF;
            uint32_t funcIdx = resolveFuncOperand(extraData);
            const char* funcName = (dw->func.functionCount > funcIdx) ? dw->func.functions[funcIdx].name : "???";
            snprintf(operandStr, operandSize, "%s(%d)", funcName, argCount);
            if (argCount > 0) {
                char argList[128] = "";
                int32_t pos = 0;
                for (int32_t i = 0; 8 > i && argCount > i; i++) {
                    if (i > 0) pos += snprintf(argList + pos, sizeof(argList) - pos, ", ");
                    pos += snprintf(argList + pos, sizeof(argList) - pos, "arg%d", i);
                }
                if (argCount > 8) snprintf(argList + pos, sizeof(argList) - pos, ", ...");
                snprintf(commentStr, commentSize, "// pops: [%s] -> pushes: [result]", argList);
            } else {
                snprintf(commentStr, commentSize, "// pushes: [result]");
            }
            break;
        }

        // Dynamic call through variable/method reference (BC17+)
        case OP_CALLV: {
            int32_t argCount = instr & 0xFFFF;
            snprintf(opcodeStr, opcodeSize, "CallV.v");
            snprintf(operandStr, operandSize, "%d", argCount);
            snprintf(commentStr, commentSize, "// pops: [func, instance, %d args] -> pushes: [result]", argCount);
            break;
        }

        // Duplicate stack items
        case OP_DUP: {
            uint8_t extra = (uint8_t) (instr & 0xFF);
            int32_t count = (int32_t) extra + 1;
            snprintf(opcodeStr, opcodeSize, "Dup.%c", gmlTypeChar(type1));
            if (count > 1) {
                snprintf(operandStr, operandSize, "%d", count);
                snprintf(commentStr, commentSize, "// duplicates %d items", count);
            } else {
                snprintf(commentStr, commentSize, "// duplicates top item");
            }
            break;
        }

        // Control flow
        case OP_RET:
            snprintf(opcodeStr, opcodeSize, "Ret.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [value] (return)");
            break;
        case OP_EXIT:
            snprintf(opcodeStr, opcodeSize, "Exit.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// (end of code)");
            break;
        case OP_POPZ:
            snprintf(opcodeStr, opcodeSize, "Popz.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [value]");
            break;

        // Break (extended opcodes in V17+)
        case OP_BREAK: {
            int16_t breakType = (int16_t) instType;
            const char* mnemonic;
            switch (breakType) {
                case BREAK_CHKINDEX:    mnemonic = "chkindex"; break;
                case BREAK_PUSHAF:      mnemonic = "pushaf"; break;
                case BREAK_POPAF:       mnemonic = "popaf"; break;
                case BREAK_PUSHAC:      mnemonic = "pushac"; break;
                case BREAK_SETOWNER:    mnemonic = "setowner"; break;
                case BREAK_ISSTATICOK:  mnemonic = "isstaticok"; break;
                case BREAK_SETSTATIC:   mnemonic = "setstatic"; break;
                case BREAK_SAVEAREF:    mnemonic = "savearef"; break;
                case BREAK_RESTOREAREF: mnemonic = "restorearef"; break;
                default:                mnemonic = nullptr; break;
            }
            if (mnemonic != nullptr) {
                snprintf(opcodeStr, opcodeSize, "%s.%c", mnemonic, gmlTypeChar(type1));
            } else {
                snprintf(opcodeStr, opcodeSize, "Break.%c", gmlTypeChar(type1));
                snprintf(operandStr, operandSize, "%d", (int32_t) breakType);
            }
            break;
        }

        default:
            snprintf(opcodeStr, opcodeSize, "??? (0x%02X)", opcode);
            break;
    }
}

void VM_buildCrossReferences(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    ctx->crossRefMap = nullptr;

    repeat(dw->code.count, callerIdx) {
        CodeEntry* code = &dw->code.entries[callerIdx];
        const uint8_t* base = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
        uint32_t ip = 0;

        while (code->length > ip) {
            uint32_t instr = BinaryUtils_readUint32(base + ip);
            ip += 4;
            const uint8_t* ed = base + ip;
            if (instrHasExtraData(instr)) {
                ip += extraDataSize(instrType1(instr));
            }

            if (instrOpcode(instr) == OP_CALL) {
                uint32_t funcIdx = resolveFuncOperand(ed);
                if (dw->func.functionCount > funcIdx) {
                    const char* funcName = dw->func.functions[funcIdx].name;
                    ptrdiff_t codeMapIdx = shgeti(ctx->funcMap, (char*) funcName);
                    if (codeMapIdx >= 0) {
                        int32_t targetIdx = ctx->funcMap[codeMapIdx].value;
                        ptrdiff_t mapIdx = hmgeti(ctx->crossRefMap, targetIdx);
                        if (0 > mapIdx) {
                            int32_t* callers = nullptr;
                            arrput(callers, (int32_t) callerIdx);
                            hmput(ctx->crossRefMap, targetIdx, callers);
                        } else {
                            // Deduplicate: don't add the same caller twice
                            int32_t* callers = ctx->crossRefMap[mapIdx].value;
                            bool found = false;
                            for (ptrdiff_t k = 0; arrlen(callers) > k; k++) {
                                if (callers[k] == (int32_t) callerIdx) { found = true; break; }
                            }
                            if (!found) {
                                arrput(ctx->crossRefMap[mapIdx].value, (int32_t) callerIdx);
                            }
                        }
                    }
                }
            }
        }
    }
}

void VM_disassemble(VMContext* ctx, int32_t codeIndex) {
    DataWin* dw = ctx->dataWin;
    require(dw->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &dw->code.entries[codeIndex];

    // Header
    printf("=== %s (length=%u, locals=%u, args=%u) ===\n", code->name, code->length, code->localsCount, code->argumentsCount);

    // CodeLocals
    CodeLocals* locals = resolveCodeLocals(ctx, code->name);
    if (locals != nullptr && locals->localVarCount > 0) {
        printf("Locals:");
        repeat(locals->localVarCount, i) {
            if (i > 0) printf(",");
            printf(" [%u] %s", locals->locals[i].varID, locals->locals[i].name);
        }
        printf("\n");
    }

    // Cross-references
    if (ctx->crossRefMap != nullptr) {
        ptrdiff_t mapIdx = hmgeti(ctx->crossRefMap, codeIndex);
        if (mapIdx >= 0) {
            int32_t* callers = ctx->crossRefMap[mapIdx].value;
            printf("Called by:");
            for (ptrdiff_t i = 0; arrlen(callers) > i; i++) {
                if (i > 0) printf(",");
                printf(" %s", dw->code.entries[callers[i]].name);
            }
            printf("\n");
        }
    }

    printf("\n");

    const uint8_t* bytecodeBase = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
    uint32_t codeLength = code->length;

    // Pass 1: collect branch targets for labels
    struct { uint32_t key; bool value; }* branchTargets = nullptr;
    {
        uint32_t ip = 0;
        while (codeLength > ip) {
            uint32_t instrAddr = ip;
            uint32_t instr = BinaryUtils_readUint32(bytecodeBase + ip);
            ip += 4;
            if (instrHasExtraData(instr)) {
                ip += extraDataSize(instrType1(instr));
            }
            uint8_t opcode = instrOpcode(instr);
            if (opcode == OP_B || opcode == OP_BT || opcode == OP_BF || opcode == OP_PUSHENV) {
                int32_t offset = instrJumpOffset(instr);
                uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                hmput(branchTargets, target, true);
            }
            if (opcode == OP_POPENV) {
                if ((instr & 0x00FFFFFF) != 0xF00000) {
                    int32_t offset = instrJumpOffset(instr);
                    uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                    hmput(branchTargets, target, true);
                }
            }
        }
    }

    // Pass 2: print instructions
    uint32_t ip = 0;
    int32_t envDepth = 0;

    while (codeLength > ip) {
        uint32_t instrAddr = ip;
        uint32_t instr = BinaryUtils_readUint32(bytecodeBase + ip);
        ip += 4;
        const uint8_t* extraData = bytecodeBase + ip;
        if (instrHasExtraData(instr)) {
            ip += extraDataSize(instrType1(instr));
        }

        uint8_t opcode = instrOpcode(instr);

        // PopEnv decreases depth before printing
        if (opcode == OP_POPENV && envDepth > 0) envDepth--;

        // Print label if this address is a branch target
        if (hmgeti(branchTargets, instrAddr) >= 0) {
            printf("  %04X: L_%04X:\n", instrAddr, instrAddr);
        }

        int32_t indent = 2 + envDepth * 4;
        char opcodeStr[32];
        char operandStr[256] = "";
        char commentStr[128] = "";

        formatInstruction(ctx, bytecodeBase, instrAddr, instr, extraData, opcodeStr, sizeof(opcodeStr), operandStr, sizeof(operandStr), commentStr, sizeof(commentStr));

        // Print the formatted line
        if (commentStr[0] != '\0') {
            printf("%*s%04X: [0x%08X] %-16s %-45s %s\n", indent, "", instrAddr, instr, opcodeStr, operandStr, commentStr);
        } else {
            printf("%*s%04X: [0x%08X] %-16s %s\n", indent, "", instrAddr, instr, opcodeStr, operandStr);
        }

        // PushEnv increases depth after printing
        if (opcode == OP_PUSHENV) envDepth++;
    }

    hmfree(branchTargets);
    printf("\n");
}

void VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func) {
    requireMessage(shgeti(ctx->builtinMap, name) == -1, "Trying to register an already registered builtin function!");
    shput(ctx->builtinMap, (char*) name, func);
}

BuiltinFunc VM_findBuiltin(VMContext* ctx, const char* name) {
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    return ctx->builtinMap[idx].value;
}

void VM_free(VMContext* ctx) {
    if (ctx == nullptr) return;

    // Reset mutable runtime state
    VM_reset(ctx);

    // Free profiler (no-op if never enabled)
    Profiler_destroy(ctx->profiler);
    ctx->profiler = nullptr;

#ifdef ENABLE_VM_OPCODE_PROFILER
    free(ctx->opcodeVariantCounts);
    ctx->opcodeVariantCounts = nullptr;
    free(ctx->opcodeRValueTypeCounts);
    ctx->opcodeRValueTypeCounts = nullptr;
#endif

    // Free global vars array itself
    free(ctx->globalVars);

    // Free hash maps
    shfree(ctx->funcMap);
    shfree(ctx->globalVarNameMap);
    shfree(ctx->selfVarNameMap);
    repeat(shlen(ctx->codeLocalsMap), i) {
        free(ctx->codeLocalsMap[i].key);
    }
    shfree(ctx->codeLocalsMap);

    // Free dedup key strings before freeing the hashmaps
    repeat(shlen(ctx->loggedUnknownFuncs), i) {
        free(ctx->loggedUnknownFuncs[i].key);
    }
    shfree(ctx->loggedUnknownFuncs);
    repeat(shlen(ctx->loggedStubbedFuncs), i) {
        free(ctx->loggedStubbedFuncs[i].key);
    }
    shfree(ctx->loggedStubbedFuncs);
#ifdef ENABLE_VM_TRACING
    shfree(ctx->varReadsToBeTraced);
    shfree(ctx->varWritesToBeTraced);
    shfree(ctx->functionCallsToBeTraced);
    shfree(ctx->alarmsToBeTraced);
    shfree(ctx->instanceLifecyclesToBeTraced);
    shfree(ctx->eventsToBeTraced);
    shfree(ctx->opcodesToBeTraced);
    shfree(ctx->stackToBeTraced);
#endif

    // Free function call cache
    free(ctx->funcCallCache);

    // Free cross-reference map
    if (ctx->crossRefMap != nullptr) {
        for (ptrdiff_t i = 0; hmlen(ctx->crossRefMap) > i; i++) {
            arrfree(ctx->crossRefMap[i].value);
        }
        hmfree(ctx->crossRefMap);
    }

    // Free builtin map
    shfree(ctx->builtinMap);
    ctx->registeredBuiltinFunctions = false;

    // Free V17+ static tracking
    free(ctx->staticInitialized);

    // Free per-code varID -> slot maps (BC17+ only; nullptr otherwise).
    if (ctx->codeLocalsSlotMaps != nullptr) {
        repeat(ctx->dataWin->code.count, i) {
            IntIntHashMap_free(&ctx->codeLocalsSlotMaps[i]);
        }
        free(ctx->codeLocalsSlotMaps);
        ctx->codeLocalsSlotMaps = nullptr;
    }

#ifndef PLATFORM_PS2
    free(ctx);
#endif
}
