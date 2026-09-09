#include "vm.h"
#include "vm_builtins.h"
#include "instance.h"
#include "runner.h"
#include "binary_utils.h"
#include "utils.h"
#include "wad_versions.h"
#include "profiler.h"
#include "string_builder.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

#include "stb_ds.h"

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

// Returns the native byte size of a GML data type on the runner's stack.
// The Dup instruction (and several BC17+ BREAK sub-opcodes) encode byte counts, not slot counts.
static int gmlTypeNativeSize(uint8_t gmlType) {
    switch (gmlType) {
        case GML_TYPE_DOUBLE:   return 8;
        case GML_TYPE_FLOAT:    return 4;
        case GML_TYPE_INT32:    return 4;
        case GML_TYPE_INT64:    return 8;
        case GML_TYPE_BOOL:     return 4;
        case GML_TYPE_VARIABLE: return 16;
        case GML_TYPE_STRING:   return 4;
        case GML_TYPE_INT16:    return 4;
        default:                return 16;
    }
}

static void stackPush(VMContext* ctx, RValue val) {
    require(VM_STACK_SIZE > ctx->stack.top);
#ifdef ENABLE_VM_TRACING
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        ctx->stack.slots[ctx->stack.top++] = val;
        char* stackBuf = formatStackContents(ctx);
        logInfo("VM: [%s] PUSH %s [stack=%d -> %d] %s\n", ctx->currentCodeName, valStr, ctx->stack.top - 1, ctx->stack.top, stackBuf);
        free(stackBuf);
        free(valStr);
        return;
    }
#endif
    ctx->stack.slots[ctx->stack.top++] = val;
}

static void stackPushTyped(VMContext* ctx, RValue val, uint8_t gmlStackType) {
    val.gmlStackType = gmlStackType;
    stackPush(ctx, val);
}

static RValue stackPop(VMContext* ctx) {
    require(ctx->stack.top > 0);
    RValue val = ctx->stack.slots[--ctx->stack.top];
#ifdef ENABLE_VM_TRACING
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        char* stackBuf = formatStackContents(ctx);
        logInfo("VM: [%s] POP  %s [stack=%d -> %d] %s\n", ctx->currentCodeName, valStr, ctx->stack.top + 1, ctx->stack.top, stackBuf);
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

#if IS_WAD17_OR_HIGHER_ENABLED

static RValue* stackPeek(VMContext* ctx) {
    require(ctx->stack.top > 0);
    return &ctx->stack.slots[ctx->stack.top - 1];
}

#endif

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

            uint32_t instrWord = BinaryUtils_readUint32(&buf[addr - base]);
            bool isPushRef = instrOpcode(instrWord) == OP_BREAK && instrInstanceType(instrWord) == BREAK_PUSHREF;

            uint32_t delta;
            if (isPushRef) {
                delta = operand & 0x00FFFFFF;
                BinaryUtils_writeUint32(&buf[operandAddr - base], ((uint32_t) ASSET_TYPE_SCRIPT << 24) | (funcIdx & 0x00FFFFFF));
            } else {
                delta = operand & 0x07FFFFFF;
                BinaryUtils_writeUint32(&buf[operandAddr - base], funcIdx);
            }

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
// Writes (VARTYPE_ARRAY Pop, BREAK_POPAF, BREAK_PUSHAC materialisation) go through VM_arraySetWithCoW,
// which handles:
//   * slot-not-yet-an-array -> allocate a fresh GMLArray
//   * CoW fork when another scope/slot owns the array (BC16 predicate uses the slot address; BC17+ predicate compares against ctx->currentArrayOwner set by BREAK_SETOWNER)
//   * grow-on-write past the current length
//   * transfer ownership of "val" into arr->data[index], freeing whatever was there before.
//

// CoW fork for array writes (BC17+): when the slot's array is owned by a different scope, replace it with a uniquely-owned clone stamped with the current owner,
// so the upcoming store (Pop for one dimensional arrays, or the chain's eventual BREAK_POPAF for multi-dimensional arrays) can write in place.
// Essentially "If we don't own this array, copy it and decrease the reference count of the original array".
// (See GameMaker-HTML5's "__yy_gml_array_check" / "__yy_gml_array_check_index_chain").
static void cowForkSlotForModificationIfNeeded(VMContext* ctx, RValue* slot) {
    GMLArray* arr = slot->array;
    if (arr->owner == ctx->currentArrayOwner) return;
    GMLArray* clone = GMLArray_clone(arr, ctx->currentArrayOwner);
    GMLArray_decRef(arr);
    slot->array = clone;
    slot->ownsReference = true;
}

// Write array[index] = val with CoW semantics. Always makes an independent copy of val, caller retains ownership and must RValue_free(&val) when done.
// `slot` is the RValue* holding the array (e.g. &globalVars[id], &inst->selfVars[..].value, &localVars[slot]).
// Returns the (possibly newly-forked) GMLArray* now in *slot.
static GMLArray* VM_arraySetWithCoW(VMContext* ctx, RValue* slot, int32_t index, RValue val) {
    require(slot != nullptr);
    requireMessageFormatted(__FILE__, __LINE__, index >= 0, "Trying to write to an array using a negative index! Index: %d", index);

    RValue writeVal = RValue_makeIndependent(val);

    void* intendedOwner;
#if IS_WAD17_OR_HIGHER_ENABLED
    intendedOwner = IS_WAD17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
#else
    (void)ctx;
    intendedOwner = (void*) slot;
#endif

    // Case 1: slot doesn't hold an array yet, replace whatever's there with a fresh one.
    if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
        RValue_free(slot);
        GMLArray* fresh = GMLArray_create(ctx->dataWin, 0);
        fresh->owner = intendedOwner;
        *slot = RValue_makeArray(fresh);
        GMLArray_growTo(fresh, index + 1);
        RValue_writeIntoSlotStealingOwnershipOrCopying(GMLArray_slot(fresh, index), writeVal);
        return fresh;
    }

    GMLArray* arr = slot->array;

    // Case 2: CoW fork check.
    if (IS_WAD17_OR_HIGHER(ctx)) {
        cowForkSlotForModificationIfNeeded(ctx, slot);
        arr = slot->array;
    } else if (arr->refCount > 1 && arr->owner != (void*) slot) {
        GMLArray* clone = GMLArray_clone(arr, intendedOwner);
        GMLArray_decRef(arr);
        slot->array = clone;
        slot->ownsReference = true;
        arr = clone;
    } else if (arr->owner == nullptr) {
        // Claim ownership on first write to an unowned array (e.g. freshly allocated by a builtin).
        arr->owner = intendedOwner;
    }

    // Case 3: Write!
    GMLArray_growTo(arr, index + 1);
    RValue_writeIntoSlotStealingOwnershipOrCopying(GMLArray_slot(arr, index), writeVal);
    return arr;
}

// Creates a copy of "name"
int32_t VM_getOrAllocateVarID(VMContext* ctx, const char* name) {
    ptrdiff_t slot = shgeti(ctx->varNameMap, name);
    if (slot >= 0) return ctx->varNameMap[slot].value;
    int32_t id = ctx->nextDynamicVarID++;
    shput(ctx->varNameMap, safeStrdup(name), id);
    return id;
}

char* VM_getVariableNameByVarId(VMContext* ctx, int32_t varId) {
    char* name = nullptr;
    repeat(shlen(ctx->varNameMap), j) {
        if (ctx->varNameMap[j].value == varId) {
            name = ctx->varNameMap[j].key;
        }
    }
    return name;
}

RValue VM_structGetVariableByVarId(Instance* structInst, int32_t slotId, int32_t arrayIndex) {
    requireMessageFormatted(__FILE__, __LINE__, structInst->objectIndex == STRUCT_OBJECT_INDEX, "Trying to use VM_structGetByVarId on a instance that isn't a struct! objectIndex=%d", structInst->objectIndex);
    RValue* slot = IntRValueHashMap_findSlot(&structInst->selfVars, slotId);
    if (slot != nullptr) {
        if (arrayIndex >= 0) return GMLArray_getOnArrayRef(slot, arrayIndex);
        RValue result = *slot;
        result.ownsReference = false;
        return result;
    }
    return RValue_makeUndefined();
}

// Plain member read on a struct.
// Returns a weak view (or undefined when the member/element doesn't exist).
RValue VM_structGetVariableByVarName(VMContext* ctx, Instance* structInst, const char* name, int32_t arrayIndex) {
    requireMessageFormatted(__FILE__, __LINE__, structInst->objectIndex == STRUCT_OBJECT_INDEX, "Trying to use VM_structGet on a instance that isn't a struct! objectIndex=%d", structInst->objectIndex);
    ptrdiff_t nameSlot = shgeti(ctx->varNameMap, (char*) name);
    if (nameSlot >= 0) {
        return VM_structGetVariableByVarId(structInst, ctx->varNameMap[nameSlot].value, arrayIndex);
    }
    return RValue_makeUndefined();
}

// Creates a copy of "name"
// This should ONLY be used for structs!
void VM_structSet(VMContext* ctx, Instance* structInst, const char* name, RValue val, int32_t arrayIndex) {
    requireMessageFormatted(__FILE__, __LINE__, structInst->objectIndex == STRUCT_OBJECT_INDEX, "Trying to use VM_structSet on a instance that isn't a struct! objectIndex=%d", structInst->objectIndex);
    int32_t varID = VM_getOrAllocateVarID(ctx, name);
    if (arrayIndex >= 0) {
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&structInst->selfVars, varID);
        VM_arraySetWithCoW(ctx, slot, arrayIndex, val);
    } else {
        Instance_setSelfVar(structInst, varID, val);
    }
}

// Creates a copy of "name"
// This should ONLY be used for structs!
void VM_structSetAndFreeVal(VMContext* ctx, Instance* structInst, const char* name, RValue val, int32_t arrayIndex) {
    VM_structSet(ctx, structInst, name, val, arrayIndex);
    RValue_free(&val);
}

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
    ArrayAccess ret = {0};
    if (varType == VARTYPE_ARRAY) {
        // For array reads, GMS pushes: instanceType then arrayIndex (arrayIndex on top)
        int32_t arrayIndex = stackPopInt32(ctx);
        int32_t instanceType = stackPopInt32(ctx);

        // BC17: if instanceType is -9 (INSTANCE_STACKTOP), the actual instance is the next stack item.
        // This is used for chained access like `command_actor[i].specialsprite[arg]` where the array variable's owning instance is resolved from a computed value on the stack.
        if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
            instanceType = resolveInstanceStackTop(ctx);
        }

        ret.arrayIndex = arrayIndex;
        ret.instanceType = instanceType;
        ret.isArray = true;
        ret.hasInstanceType = true;
        return ret;
    }
    if (varType == VARTYPE_STACKTOP) {
        int32_t instanceType = stackPopInt32(ctx);

        // BC17: PushI.e -9 (INSTANCE_STACKTOP) is pushed before the Pop instruction.
        // When we pop -9, it means "the real instance type is the next item on the stack".
        if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
            instanceType = resolveInstanceStackTop(ctx);
        }
        ret.arrayIndex = -1;
        ret.isArray = false;
        ret.hasInstanceType = true;
        ret.instanceType = instanceType;
        return ret;
    }
    ret.arrayIndex = -1;
    ret.isArray = false;
    ret.hasInstanceType = false;
    return ret;
}

// ===[ Variable Resolution ]===

// Returns the object name for an instance, or "<global_scope>" for the global scope dummy instance
#ifdef ENABLE_VM_TRACING

static const char* instanceObjectName(VMContext* ctx, Instance* inst) {
    if (inst->objectIndex == STRUCT_OBJECT_INDEX) return "<global_scope>";
    return ctx->dataWin->objt.objects[inst->objectIndex].name;
}

#endif

static Variable* resolveVarDef(VMContext* ctx, uint32_t varRef) {
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    return varDef;
}

// Maps a GML local's varID to its slot position in the current code's localVars[] array.
//
// BC15/16: varIDs for locals are already sequential slot indices (0, 1, 2, ...), so we return the varID unchanged.
//
// BC13/BC14, BC17+: a single GML local can surface as several VARI chunk entries that share a varID (BC17+) or has no pre-assigned slot at all (BC13/BC14).
// We key by varID/varIdx via the per-code currentCodeLocalsSlotMap so reads/writes via any reference agree on the same localVars slot.
static uint32_t resolveLocalSlot(VMContext* ctx, int32_t varID) {
    if (IS_WAD15_OR_HIGHER(ctx) && IS_WAD16_OR_BELOW(ctx)) {
        return (uint32_t) varID;
    }

    // BC13/BC14 and BC17+: allocate the slot dynamically because the data.win CANNOT be trusted to know how many localVars the script has
    uint32_t slot = IntIntHashMap_getOrInsertSequential(ctx->currentCodeLocalsSlotMap, varID);

    // Grow this frame's localVars window to cover `slot` whether the entry is pre-existing or freshly allocated.
    // Pre-existing entries can still be past ctx->localVarCount if a nested call to the same code extended the slot map while the outer frame was suspended (the outer frame's localVarCount is captured at call entry and doesn't follow later growth).
    if (slot >= ctx->localVarCount) {
        RValue* resizedLocalVars = (RValue *)safeCalloc(slot + 1, sizeof(RValue));
        memcpy(resizedLocalVars, ctx->localVars, sizeof(RValue) * ctx->localVarCount);
        free(ctx->localVars);
        ctx->localVars = resizedLocalVars;
        ctx->localVarCount = slot + 1;
    }
    return slot;
}

// Finds an instance by target value.
// target >= INSTANCE_ID_BASE: instance ID (find specific instance, including recently-destroyed-but-not-cleaned-up-yet ones so GML code can read properties of an instance just after instance_destroy within the same step).
// target >= 0 && target < INSTANCE_ID_BASE: object index (find first ACTIVE instance of that object, checking parent chains)
Instance* VM_findInstanceByTarget(VMContext* ctx, int32_t target) {
    Runner* runner = (Runner*) ctx->runner;

    if (target >= INSTANCE_ID_BASE) {
        // Instance ID - find specific instance
        return hmget(runner->instancesById, target);
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
            if (slot == nullptr)
                return false;
            *out = *slot;
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
            Instance* inst = (Instance*) ctx->globalScopeInstance;
            RValue* slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            if (slot == nullptr)
                return false;
            *out = *slot;
            out->ownsReference = false;
            return true;
        }
        case INSTANCE_OTHER: {
            Instance* inst = (Instance*) ctx->otherInstance;
            if (inst == nullptr) return false;
            RValue* slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            if (slot == nullptr)
                return false;
            *out = *slot;
            out->ownsReference = false;
            return true;
        }
    }
    return false;
}

#if IS_WAD17_OR_HIGHER_ENABLED
// Static variables: Each code index has its own "struct" for static variables.
// Lazily create a struct for each codeIndex that needs a static variable.
Instance* VM_getOrCreateStaticStruct(VMContext* ctx, int32_t codeIndex) {
    if (ctx->staticStructs == nullptr || 0 > codeIndex || (uint32_t) codeIndex >= ctx->dataWin->code.count) return nullptr;
    Instance* staticStruct = ctx->staticStructs[codeIndex];
    if (staticStruct == nullptr) {
        staticStruct = Runner_createStruct((Runner*) ctx->runner);
        staticStruct->pinned = true;
        ctx->staticStructs[codeIndex] = staticStruct;
    }
    return staticStruct;
}

// Used when the static variable could not be read from the original struct, walks through the chain to find who is the owner of the static variable.
// Returns true and fills *out when found.
static bool tryReadStaticFallback(VMContext* ctx, Instance* inst, int32_t varID, ArrayAccess* access, RValue* out) {
    if (ctx->staticStructs == nullptr || inst == nullptr || inst->objectIndex != STRUCT_OBJECT_INDEX) return false;
    if (0 > inst->constructorCodeIndex || (uint32_t) inst->constructorCodeIndex >= ctx->dataWin->code.count) return false;
    Instance* staticStruct = ctx->staticStructs[inst->constructorCodeIndex];
    int32_t depth = 0;
    // Walk instance's static struct -> parent static -> ...
    while (staticStruct != nullptr) {
        requireMessage(64 > depth, "Try read static fallback chain is too deep! Bug?");
        RValue* sslot = IntRValueHashMap_findSlot(&staticStruct->selfVars, varID);
        if (sslot != nullptr) {
            if (access->isArray) {
                *out = GMLArray_getOnArrayRef(sslot, access->arrayIndex);
            } else {
                *out = *sslot;
                out->ownsReference = false;
            }
            return true;
        }
        staticStruct = staticStruct->staticParent;

        depth++;
    }
    return false;
}

// Links the current constructor's static struct to a parent constructor's static struct for static inheritance.
void VM_copyStatic(VMContext* ctx, RValue* parentRef) {
    if (ctx->staticStructs == nullptr) return;
    // Resolve the parent constructor's code index (FUNC index -> name -> codeIndex), mirroring @@NewGMLObject@@.
    int32_t parentCodeIndex = -1;
    if (parentRef->type == RVALUE_METHOD && parentRef->method != nullptr) {
        parentCodeIndex = parentRef->method->codeIndex;
    } else {
        int32_t rawArg = RValue_toInt32(*parentRef);
        if (rawArg >= 0 && ctx->dataWin->func.functionCount > (uint32_t) rawArg) {
            const char* funcName = ctx->dataWin->func.functions[rawArg].name;
            if (funcName != nullptr) {
                ptrdiff_t idx = shgeti(ctx->codeIndexByName, (char*) funcName);
                if (idx >= 0) parentCodeIndex = ctx->codeIndexByName[idx].value;
            }
        }
    }
    if (0 > parentCodeIndex) return;
    Instance* childStatic = VM_getOrCreateStaticStruct(ctx, ctx->currentCodeIndex);
    Instance* parentStatic = VM_getOrCreateStaticStruct(ctx, parentCodeIndex);
    if (childStatic != nullptr && parentStatic != nullptr && childStatic != parentStatic) {
        childStatic->staticParent = parentStatic;
    }
}
#endif

// Tries reading a instance variable (from the selfVars map) and, if it doesn't exist, tries reading from the shared static struct.
static bool tryReadInstanceVarOrStatic(VMContext* ctx, Instance* instance, int32_t varID, ArrayAccess* access, RValue* out) {
    RValue* slot = IntRValueHashMap_findSlot(&instance->selfVars, varID);
    if (slot != nullptr) {
        if (access->isArray) {
            *out = GMLArray_getOnArrayRef(slot, access->arrayIndex);
            return true;
        }
        RValue val = *slot;
        val.ownsReference = false;
        *out = val;
        return true;
    }

#if IS_WAD17_OR_HIGHER_ENABLED
    // Static variables: a struct field declared "static" lives on the constructor's shared static struct, not the instance.
    RValue staticVal;
    if (tryReadStaticFallback(ctx, instance, varID, access, &staticVal)) {
        *out = staticVal;
        return true;
    }
#else
    (void)ctx;
#endif
    return false;
}

static inline bool VM_ensureScriptArg(VMContext* ctx, int32_t writeIndex) {
    if (writeIndex < 0) return false;
    if (writeIndex < ctx->scriptArgCount) return true;

    RValue* newScriptArgs = (RValue *)safeCalloc(writeIndex + 1, sizeof(RValue));

    if (ctx->scriptArgCount > 0) {
        memcpy(newScriptArgs, ctx->scriptArgs, ctx->scriptArgCount * sizeof(RValue));
        free(ctx->scriptArgs);
    }

    ctx->scriptArgs = newScriptArgs;
    ctx->scriptArgCount = writeIndex + 1;
    return true;
}

void VM_writeToScriptArgs(VMContext* ctx, int32_t writeIndex, RValue val) {
    if (!VM_ensureScriptArg(ctx, writeIndex)) return;

    RValue independent = RValue_makeIndependent(val);
    RValue_free(&ctx->scriptArgs[writeIndex]);
    ctx->scriptArgs[writeIndex] = independent;
}

void VM_writeToScriptArgsArrayElement(VMContext* ctx, int32_t writeIndex, int32_t arrayIndex, RValue val) {
    if (!VM_ensureScriptArg(ctx, writeIndex)) return;
    VM_arraySetWithCoW(ctx, &ctx->scriptArgs[writeIndex], arrayIndex, val);
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
#if IS_WAD17_OR_HIGHER_ENABLED
    if (IS_WAD17_OR_HIGHER(ctx) && !access.hasInstanceType && instanceType == INSTANCE_STACKTOP) {
        instanceType = resolveInstanceStackTop(ctx);
    }
#endif

    // Resolve target instance for object/instance references (instanceType >= 0)
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = VM_findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
            if (instanceType < INSTANCE_ID_BASE && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                logWarn("VM: [%s] READ var '%s' on object index %d (%s) but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            } else {
                logWarn("VM: [%s] READ var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            }
            return RValue_makeReal(0.0);
        }
    } else if (instanceType == INSTANCE_GLOBAL) {
        targetInstance = ctx->globalScopeInstance;
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
#if IS_WAD17_OR_HIGHER_ENABLED
    } else if (instanceType == INSTANCE_STATIC) {
        // "static" scope: read from the current constructor's shared static struct via the normal slot path below.
        targetInstance = VM_getOrCreateStaticStruct(ctx, ctx->currentCodeIndex);
#endif
    } else if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_ARG) {
        // BC17: argument0..argument15 via INSTANCE_ARG instance type (builtinVarId pre-resolved at parse time)
        int16_t builtinVarId = varDef->builtinVarId;
        RValue result;
        if (builtinVarId == BUILTIN_VAR_ARGUMENT_COUNT) {
            result = RValue_makeReal((GMLReal) ctx->scriptArgCount);
        } else if (builtinVarId == BUILTIN_VAR_ARGUMENT) {
            // argument[N] array-style access
            int32_t idx = access.arrayIndex;
            if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > idx && idx >= 0) {
                result = ctx->scriptArgs[idx];
                result.ownsReference = false;
            } else {
                result = RValue_makeUndefined();
            }
        } else if (builtinVarId >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= builtinVarId) {
            int32_t argIndex = builtinVarId - BUILTIN_VAR_ARGUMENT0;
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
                    result = GMLArray_getOnArrayRef(&result, access.arrayIndex);
                }
            } else {
                result = RValue_makeUndefined();
            }
        } else {
            logWarn("VM: [%s] INSTANCE_ARG read on unknown variable '%s' (builtinVarId=%d)\n", ctx->currentCodeName, varDef->name, builtinVarId);
            result = RValue_makeUndefined();
        }
        return result;
    }

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == VARIABLE_BUILTIN) {
        // Structs aren't real game instances, but structs CAN store fields with the same names as built-ins.
        // So we'll check the self variables FIRST before checking for built-ins.
        if (targetInstance != nullptr && targetInstance->objectIndex == STRUCT_OBJECT_INDEX) {
            ptrdiff_t nameSlot = shgeti(ctx->varNameMap, (char*) varDef->name);
            if (nameSlot >= 0) {
                int32_t structVarID = ctx->varNameMap[nameSlot].value;
                RValue value;
                if (tryReadInstanceVarOrStatic(ctx, targetInstance, structVarID, &access, &value))
                    return value;
            }
        }

        RValue result = VMBuiltins_getVariable(ctx, targetInstance, varDef->builtinVarId, varDef->name, access.arrayIndex);

#ifdef ENABLE_VM_TRACING
        if (instanceType == INSTANCE_GLOBAL) {
            VM_checkIfVariableShouldBeTracedAndLog(ctx, "global", nullptr, varDef->name, result, false, access.arrayIndex, -1, " (builtin)");
        } else if (targetInstance != nullptr && targetInstance->objectIndex >= 0) {
            VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, targetInstance), "self", varDef->name, result, false, access.arrayIndex, targetInstance->instanceId, " (builtin)");
        }
#endif

        return result;
    }

    // Resolve the variable's scalar slot pointer for the target scope. Array-valued vars live inline as RVALUE_ARRAY in the same slot.
    // GMLArray_getOnArrayRef handles the array indirection when access.isArray, VM_arraySetWithCoW handles CoW forking when writing.
    RValue* slot = nullptr;
    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            slot = &ctx->localVars[localSlot];
            break;
        }
        case INSTANCE_SELF:
        case INSTANCE_GLOBAL:
        default: {
            Instance* inst = targetInstance;
            if (inst == nullptr) {
                const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
                logWarn("VM: [%s] Read on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
                return RValue_makeReal(0.0);
            }
            slot = IntRValueHashMap_findSlot(&inst->selfVars, varDef->varID);
            // sparse storage: nonexistent entry -> treat as undefined scalar (array reads fall through to GMLArray_getOnArrayRef returning undefined)
            if (slot == nullptr) {
#if IS_WAD17_OR_HIGHER_ENABLED
                // Static variables: a struct field declared "static" lives on the constructor's shared static struct, not the instance.
                RValue staticVal;
                if (tryReadStaticFallback(ctx, inst, varDef->varID, &access, &staticVal)) {
                    return staticVal;
                }
#endif
                return RValue_makeUndefined();
            }
            break;
        }
    }

    // Array access: read array[index] from the slot.
    if (access.isArray) {
        RValue result = GMLArray_getOnArrayRef(slot, access.arrayIndex);
#ifdef ENABLE_VM_TRACING
        const char* scopeName =
            instanceType == INSTANCE_LOCAL ? "local" :
            instanceType == INSTANCE_GLOBAL ? "global" :
            (targetInstance != nullptr ? instanceObjectName(ctx, targetInstance) : "self");
        const char* altName = (instanceType == INSTANCE_SELF || instanceType >= 0 || instanceType == INSTANCE_OTHER) ? "self" : nullptr;
        VM_checkIfVariableShouldBeTracedAndLog(ctx, scopeName, altName, varDef->name, result, false, access.arrayIndex, -1, "");
#endif
        return result;
    }

    // Scalar access: return the slot's current value as a weak view (slot retains ownership).
    RValue result = *slot;
    result.ownsReference = false;

#ifdef ENABLE_VM_TRACING
    if (instanceType == INSTANCE_GLOBAL) {
        VM_checkIfVariableShouldBeTracedAndLog(ctx, "global", nullptr, varDef->name, result, false, -1, -1, "");
    } else if (instanceType == INSTANCE_SELF || instanceType >= 0) {
        Instance* inst = targetInstance;
        if (inst != nullptr) {
            VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, result, false, -1, inst->instanceId, "");
        }
    }
#endif

    return result;
}

// Helper: write a variable value to a single specific instance (always copies, never moves the original val)
static void writeSingleInstanceVariable(VMContext* ctx, Instance* inst, Variable* varDef, ArrayAccess* access, RValue val) {
    // Built-in variable (varID == -6 sentinel)
    if (varDef->varID == VARIABLE_BUILTIN) {
        VMBuiltins_setVariable(ctx, inst, varDef->builtinVarId, varDef->name, val, access->arrayIndex);
        return;
    }

    // Array write - materialise-on-write via VM_arraySetWithCoW. getOrInsertUndefined returns the existing slot or inserts an UNDEFINED entry and returns it.
    if (access->isArray) {
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
        VM_arraySetWithCoW((VMContext*) ctx, slot, access->arrayIndex, val);
        return;
    }

    // Scalar write (Instance_setSelfVar always takes an independent ref; caller still owns "val").
    Instance_setSelfVar(inst, varDef->varID, val);
}

// Force out-of-line so the OP_POP fast path in executeLoop doesn't inline this, because we already have an "optimized" version for common writes
NOINLINE
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
                RValue_writeIntoSlotStealingOwnershipOrCopying(&ctx->localVars[localSlot], val);
#ifdef ENABLE_VM_TRACING
                VM_checkIfVariableShouldBeTracedAndLog(ctx, "local", nullptr, varDef->name, ctx->localVars[localSlot], true, -1, -1, "");
#endif
                return;
            }
            case INSTANCE_SELF:
            case INSTANCE_GLOBAL: {
                Instance* inst = (Instance*) ctx->currentInstance;
                if (instanceType == INSTANCE_GLOBAL)
                    inst = ctx->globalScopeInstance;

                if (inst != nullptr) {
                    Instance_setSelfVar(inst, varDef->varID, val);
#ifdef ENABLE_VM_TRACING
                    {
                        RValue written = Instance_getSelfVar(inst, varDef->varID);
                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, written, true, -1, inst->instanceId, "");
                    }
#endif
                    RValue_free(&val);
                    return;
                }
                break; // fall through to slow path so the existing nullptr-instance error gets logged
            }
            case INSTANCE_OTHER: {
                Instance* inst = (Instance*) ctx->otherInstance;
                if (inst != nullptr) {
                    Instance_setSelfVar(inst, varDef->varID, val);
#ifdef ENABLE_VM_TRACING
                    {
                        RValue written = Instance_getSelfVar(inst, varDef->varID);
                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, written, true, -1, inst->instanceId, "");
                    }
#endif
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
#if IS_WAD17_OR_HIGHER_ENABLED
    if (IS_WAD17_OR_HIGHER(ctx) && !access.hasInstanceType && instanceType == INSTANCE_STACKTOP) {
        instanceType = resolveInstanceStackTop(ctx);
    }

    // "static" scope: write to the current constructor's shared static struct (runs once, guarded by isstaticok/setstatic).
    if (instanceType == INSTANCE_STATIC) {
        Instance* staticStruct = VM_getOrCreateStaticStruct(ctx, ctx->currentCodeIndex);
        if (staticStruct != nullptr) {
            writeSingleInstanceVariable(ctx, staticStruct, varDef, &access, val);
        }
        RValue_free(&val);
        return;
    }
#endif

    // GML: writing through an object reference (obj_foo.var = val) sets the variable on ALL instances of that object. The setter (writeSingleInstanceVariable) can run user code, so iterate a snapshot of the bucket.
    if (instanceType >= 0 && INSTANCE_ID_BASE > instanceType) {
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
            VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, val, true, -1, inst->instanceId, " (all-instances object write)");
#endif
        }
        Runner_popInstanceSnapshot(runner, snapBase);
        if (!found) {
            if (ctx->dataWin->objt.count > (uint32_t) instanceType) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                char* valAsString = RValue_toString(val, ctx->runner->dataWin);
                logWarn("VM: [%s] WRITE var '%s' on object %d (%s) but no instances found (value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, valAsString);
                free(valAsString);
            }
        }
        RValue_free(&val);
        return;
    }

    // Resolve target instance for instance ID references (instanceType >= INSTANCE_ID_BASE) or special types
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = VM_findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
            char* valAsString = RValue_toString(val, ctx->runner->dataWin);
            logWarn("VM: [%s] WRITE var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
            free(valAsString);
            return;
        }
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
    } else if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_ARG) {
        // BC17: write to argument0..argument15 via INSTANCE_ARG instance type (builtinVarId pre-resolved at parse time)
        int16_t bid = varDef->builtinVarId;
        int32_t writeIndex = -1;
        if (bid >= BUILTIN_VAR_ARGUMENT0 && BUILTIN_VAR_ARGUMENT15 >= bid) {
            writeIndex = bid - BUILTIN_VAR_ARGUMENT0;
        } else if (bid == BUILTIN_VAR_ARGUMENT) {
            writeIndex = access.arrayIndex;
        } else {
            logWarn("VM: [%s] INSTANCE_ARG write on unknown variable '%s' (builtinVarId=%d)\n", ctx->currentCodeName, varDef->name, bid);
        }
        if (writeIndex >= 0) {
            if (access.isArray) {
                VM_writeToScriptArgsArrayElement(ctx, writeIndex, access.arrayIndex, val);
            } else {
                VM_writeToScriptArgs(ctx, writeIndex, val);
            }
        }
        RValue_free(&val);
        return;
    }

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == VARIABLE_BUILTIN) {
        VMBuiltins_setVariable(ctx, targetInstance, varDef->builtinVarId, varDef->name, val, access.arrayIndex);

#ifdef ENABLE_VM_TRACING
        if (instanceType == INSTANCE_GLOBAL) {
            VM_checkIfVariableShouldBeTracedAndLog(ctx, "global", nullptr, varDef->name, val, true, access.arrayIndex, -1, " (builtin)");
        } else if (targetInstance != nullptr && targetInstance->objectIndex >= 0) {
            VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, targetInstance), "self", varDef->name, val, true, access.arrayIndex, targetInstance->instanceId, " (builtin)");
        }
#endif

        // VMBuiltins_setVariable reads values (toReal, toInt32, etc.) but does not take ownership
        RValue_free(&val);
        return;
    }

    // Resolve the slot pointer for this scope. For INSTANCE_SELF we materialise a sparse selfVars entry if it doesn't exist so VM_arraySetWithCoW has a stable slot to own.
    RValue* slot = nullptr;
    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            slot = &ctx->localVars[localSlot];
            break;
        }
        case INSTANCE_SELF:
        case INSTANCE_GLOBAL:
        default: {
            Instance* inst = targetInstance;
            if (instanceType == INSTANCE_GLOBAL)
                inst = ctx->globalScopeInstance;

            if (inst == nullptr) {
                const char* varTypeName = varTypeToString((varRef >> 24) & 0xF8);
                char* valAsString = RValue_toString(val, ctx->runner->dataWin);
                logWarn("VM: [%s] Write on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
                free(valAsString);
                RValue_free(&val);
                return;
            }
            slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
            break;
        }
    }

    // Array write via VM_arraySetWithCoW.
    if (access.isArray) {
        VM_arraySetWithCoW(ctx, slot, access.arrayIndex, val);
#ifdef ENABLE_VM_TRACING
        const char* scopeName =
            instanceType == INSTANCE_LOCAL ? "local" :
            instanceType == INSTANCE_GLOBAL ? "global" :
            (targetInstance != nullptr ? instanceObjectName(ctx, targetInstance) : "self");
        const char* altName = (instanceType == INSTANCE_SELF || instanceType >= 0 || instanceType == INSTANCE_OTHER) ? "self" : nullptr;
        VM_checkIfVariableShouldBeTracedAndLog(ctx, scopeName, altName, varDef->name, val, true, access.arrayIndex, -1, "");
#endif
        RValue_free(&val);
        return;
    }

    switch (instanceType) {
        case INSTANCE_LOCAL: {
            uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
            require(ctx->localVarCount > localSlot);
            RValue_writeIntoSlotStealingOwnershipOrCopying(&ctx->localVars[localSlot], val);
            return;
        }
        case INSTANCE_SELF:
        case INSTANCE_GLOBAL:
        default: {
            // Self or object/instance reference - use sparse hashmap
            Instance* inst = targetInstance;
            if (instanceType == INSTANCE_GLOBAL)
                inst = ctx->globalScopeInstance;
            Instance_setSelfVar(inst, varDef->varID, val);
#ifdef ENABLE_VM_TRACING
            {
                RValue written = Instance_getSelfVar(inst, varDef->varID);
                VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, written, true, -1, inst->instanceId, "");
            }
#endif
            // Instance_setSelfVar always copies strings, so free the original
            RValue_free(&val);
            return;
        }
    }
}

// ===[ Type Conversion ]===

static RValue convertValue(RValue val, uint8_t targetType, DataWin* dataWin) {
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
            char* str = RValue_toString(val, dataWin);
            return RValue_makeOwnedString(str);
        }
        case GML_TYPE_VARIABLE:
            // Variable type on stack is just an RValue passthrough
            return val;
        default:
            logWarn("VM: Unknown target type 0x%X for conversion\n", targetType);
            return val;
    }
}

// ===[ Opcode Handlers ]===

static void handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint8_t type1) {
    switch (type1) {
        case GML_TYPE_DOUBLE:
            stackPushTyped(ctx, RValue_makeReal(BinaryUtils_readFloat64Aligned(extraData)), GML_TYPE_DOUBLE);
            break;
        case GML_TYPE_FLOAT:
            // Native push.f reads a 4-byte float; the bytecode-declared footprint is FLOAT (4 bytes), not DOUBLE (8).
            stackPushTyped(ctx, RValue_makeReal((GMLReal) BinaryUtils_readFloat32Aligned(extraData)), GML_TYPE_FLOAT);
            break;
        case GML_TYPE_INT32:
            stackPushTyped(ctx, RValue_makeInt32(BinaryUtils_readInt32Aligned(extraData)), GML_TYPE_INT32);
            break;
        case GML_TYPE_INT64:
            stackPushTyped(ctx, RValue_makeInt64(BinaryUtils_readInt64Aligned(extraData)), GML_TYPE_INT64);
            break;
        case GML_TYPE_BOOL:
            stackPushTyped(ctx, RValue_makeBool(BinaryUtils_readInt32Aligned(extraData) != 0), GML_TYPE_BOOL);
            break;
        case GML_TYPE_VARIABLE: {
            int32_t instanceType = (int32_t) instrInstanceType(instr);
            uint32_t varRef = resolveVarOperand(extraData);
            uint8_t varType = (varRef >> 24) & 0xF8;
            // BC17: VARTYPE_INSTANCE encodes (instanceId - INSTANCE_ID_BASE) in the instruction's lower 16 bits.
            // Add INSTANCE_ID_BASE back so findInstanceByTarget sees the real runtime instance ID.
            if (varType == VARTYPE_INSTANCE) instanceType += INSTANCE_ID_BASE;
#if IS_WAD17_OR_HIGHER_ENABLED
            if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
                // V17: multi-dim first-step. Stack has [scope, firstIndex] (with an optional real-instance slot underneath when scope == -9 INSTANCE_STACKTOP).
                // We resolve the variable's top-level array slot, materialise it if needed, then drill into arr->data[firstIndex] (materialising a sub-array there too).
                // The sub-array is pushed as a weak ref; subsequent BREAK_PUSHAC/PUSHAF/POPAF consume it.
                Variable* varDef = resolveVarDef(ctx, varRef);
                int32_t firstIndex = stackPopInt32(ctx);
                int32_t scope = stackPopInt32(ctx);
                if (IS_WAD17_OR_HIGHER(ctx) && scope == INSTANCE_STACKTOP) {
                    scope = resolveInstanceStackTop(ctx);
                }

                // Built-in variables do NOT live in selfVars: argument[] is the call frame, not a
                // member. Falling through would call getOrInsertUndefined, find nothing, materialise
                // a FRESH EMPTY array in that slot and drill into it -- so argument[0][i] came back
                // undefined while argument[0] (which goes through resolveVariableRead) was correct.
                // Read the real value and hand out a weak ref to it instead.
                if (varDef->builtinVarId >= 0) {
                    Instance* builtinInst = (scope == INSTANCE_OTHER && ctx->otherInstance != nullptr)
                        ? (Instance*) ctx->otherInstance
                        : (scope == INSTANCE_GLOBAL) ? ctx->globalScopeInstance
                        : (scope >= 0) ? VM_findInstanceByTarget(ctx, scope)
                        : (Instance*) ctx->currentInstance;
                    RValue builtinVal = VMBuiltins_getVariable(ctx, builtinInst, varDef->builtinVarId, varDef->name, firstIndex);
                    // Only a borrowed array can become a weak ref: freeing an owned one here would
                    // leave the stack pointing at released memory. Anything else keeps the old path,
                    // so this cannot turn a working case into an abort.
                    if (builtinVal.type == RVALUE_ARRAY && builtinVal.array != nullptr && !builtinVal.ownsReference) {
                        stackPush(ctx, RValue_makeArrayWeak(builtinVal.array));
                        break;
                    }
                    RValue_free(&builtinVal);
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
                    case INSTANCE_SELF:
                    case INSTANCE_OTHER:
                    case INSTANCE_GLOBAL: {
                        Instance* inst = scope == INSTANCE_GLOBAL ? ctx->globalScopeInstance
                            : (scope == INSTANCE_OTHER && ctx->otherInstance != nullptr)
                            ? (Instance*) ctx->otherInstance
                            : (Instance*) ctx->currentInstance;
                        require(inst != nullptr);
                        slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
                        break;
                    }
                    default: {
                        // Negative pseudo-scopes not handled above resolve to the current instance, mirroring handlePushBltn.
                        // Positive scopes are real instance IDs resolved by lookup.
                        Instance* inst = (0 > scope) ? (Instance*) ctx->currentInstance : VM_findInstanceByTarget(ctx, scope);
                        if (inst == nullptr) {
                            logError("VM: ARRAYPUSHAF: no instance for scope %d varID=%d\n", scope, varDef->varID);
                            abort();
                        }
                        slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
                        break;
                    }
                }

                bool forWrite = (varType == VARTYPE_ARRAYPOPAF);
                // Materialise the top-level array in the slot if needed.
                if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
                    RValue_free(slot);
                    GMLArray* fresh = GMLArray_create(ctx->dataWin, 0);
                    fresh->owner = IS_WAD17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
                    *slot = RValue_makeArray(fresh);
                } else if (forWrite) {
                    cowForkSlotForModificationIfNeeded(ctx, slot);
                }
                GMLArray* top = slot->array;
                GMLArray_growTo(top, firstIndex + 1);
                RValue* topSlot = GMLArray_slot(top, firstIndex);
                // Materialise the sub-array at [firstIndex] if it's not already an array.
                if (topSlot->type != RVALUE_ARRAY || topSlot->array == nullptr) {
                    RValue_free(topSlot);
                    GMLArray* sub = GMLArray_create(ctx->dataWin, 0);
                    sub->owner = top->owner;
                    *topSlot = RValue_makeArray(sub);
                } else if (forWrite) {
                    cowForkSlotForModificationIfNeeded(ctx, topSlot);
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
            logError("VM: Push with unknown type 0x%X\n", type1);
            abort();
    }
}

#if IS_WAD17_OR_HIGHER_ENABLED
// For V17+ VARTYPE_ARRAYPUSHAF/POPAF on a top-level variable: return the slot's GMLArray*,
// materialising a fresh empty one in the slot if it isn't an array yet. Used by PushLoc/Glb/Bltn.
// Pushes a weak ref onto the stack — short-lived, consumed by the next BREAK_PUSHAC/PUSHAF/POPAF.
// forWrite (VARTYPE_ARRAYPOPAF) chains end in a BREAK_POPAF store, so the slot must be CoW-forked up front.
static void pushTopLevelArrayRef(VMContext* ctx, RValue* slot, bool forWrite) {
    if (slot->type != RVALUE_ARRAY || slot->array == nullptr) {
        RValue_free(slot);
        GMLArray* fresh = GMLArray_create(ctx->dataWin, 0);
        fresh->owner = IS_WAD17_OR_HIGHER(ctx) ? ctx->currentArrayOwner : (void*) slot;
        *slot = RValue_makeArray(fresh);
    } else if (forWrite) {
        cowForkSlotForModificationIfNeeded(ctx, slot);
    }
    stackPush(ctx, RValue_makeArrayWeak(slot->array));
}
#endif

static void handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    uint32_t varRef = resolveVarOperand(extraData);
#if IS_WAD17_OR_HIGHER_ENABLED
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
            inst = VM_findInstanceByTarget(ctx, scope);
        } else {
            inst = (Instance*) ctx->currentInstance;
        }
        if (inst == nullptr) {
            logError("VM: PushBltn ARRAYPUSHAF: no instance for scope %d varID=%d\n", scope, varDef->varID);
            abort();
        }
        // Same trap as in handlePush: a built-in has no selfVars slot, and materialising one there
        // would replace the real value with an empty array. PushBltn only ever names built-ins, but
        // the index of the first step is NOT on the stack here -- this opcode form carries no array
        // index, so read the variable whole (arrayIndex -1) and hand out a weak ref to it.
        if (varDef->builtinVarId >= 0) {
            RValue builtinVal = VMBuiltins_getVariable(ctx, inst, varDef->builtinVarId, varDef->name, -1);
            if (builtinVal.type == RVALUE_ARRAY && builtinVal.array != nullptr && !builtinVal.ownsReference) {
                stackPush(ctx, RValue_makeArrayWeak(builtinVal.array));
                return;
            }
            RValue_free(&builtinVal);
        }
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varDef->varID);
        pushTopLevelArrayRef(ctx, slot, varType == VARTYPE_ARRAYPOPAF);
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

// When storing into a variant variable from an int32/int64 stack source, coerce to real.
// GMS variables normalize integer literals to doubles so subsequent arithmetic routes through the real fast path instead of int32 x int32 wrapping.
static inline RValue coerceIntStoreToReal(RValue val, uint8_t type2) {
    if (type2 == GML_TYPE_INT32 || type2 == GML_TYPE_INT64 || type2 == GML_TYPE_INT16) {
        if (val.type == RVALUE_INT32) {
            return RValue_makeReal((GMLReal) val.int32);
        }
#ifndef NO_RVALUE_INT64
        if (val.type == RVALUE_INT64) {
            return RValue_makeReal((GMLReal) val.int64);
        }
#endif
    }
    return val;
}

static void handlePop(VMContext* ctx, uint8_t type1, uint8_t type2, uint32_t varRef, uint8_t varType, int32_t instanceType) {
    RValue val;
    int32_t arrayIndex = -1;

    int32_t originalInstanceType = instanceType;
    if (varType == VARTYPE_ARRAY) {
        if (type1 == GML_TYPE_VARIABLE) {
            // Simple assignment (Pop.v.v): stack bottom-to-top = [value, (realInstance,) instanceType, arrayIndex]
            arrayIndex = stackPopInt32(ctx);
            instanceType = stackPopInt32(ctx);

            // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance ID/object index" (e.g. `su_actor.specialsprite[0] = ...`)
            if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
                instanceType = resolveInstanceStackTop(ctx);
            }

            val = stackPop(ctx);
        } else {
            // Compound assignment (Pop.i.v, etc.): stack bottom-to-top = [(realInstance,) instanceType, arrayIndex, value]
            val = stackPop(ctx);

            arrayIndex = stackPopInt32(ctx);
            instanceType = stackPopInt32(ctx);

            // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance ID/object index"
            if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
                instanceType = resolveInstanceStackTop(ctx);
            }
        }
    } else if (varType == VARTYPE_STACKTOP && type1 == GML_TYPE_VARIABLE) {
        // Simple assignment (Pop.v.v) with STACKTOP: stack bottom-to-top = [value, instanceType]
        // Pop instanceType first (top), then value (bottom)
        instanceType = stackPopInt32(ctx);

        // BC17: -9 (INSTANCE_STACKTOP) means "pop again for the real instance type"
        if (IS_WAD17_OR_HIGHER(ctx) && instanceType == INSTANCE_STACKTOP) {
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
        RValue converted = convertValue(val, type1, ctx->runner->dataWin);
        RValue_free(&val);
        val = converted;
    }

    if (type1 == GML_TYPE_VARIABLE && !isCompoundAssignment) {
        val = coerceIntStoreToReal(val, type2);
    }

    if (varType == VARTYPE_ARRAY) {
        Variable* varDef = resolveVarDef(ctx, varRef);
        if (varDef->varID == VARIABLE_BUILTIN) {
            // Resolve target instance for built-in array variable writes
            if (instanceType >= 0) {
                if (INSTANCE_ID_BASE > instanceType) {
                    // Object reference: write to ALL instances of that object. The setter can run user code, so iterate a snapshot of the bucket.
                    Runner* runner = (Runner*) ctx->runner;
                    int32_t snapBase = Runner_pushInstancesOfObject(runner, instanceType);
                    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
                    for (int32_t i = snapBase; snapEnd > i; i++) {
                        Instance* inst = runner->instanceSnapshots[i];
                        if (!inst->active)
                            continue;

                        VMBuiltins_setVariable(ctx, inst, varDef->builtinVarId, varDef->name, val, arrayIndex);
#ifdef ENABLE_VM_TRACING
                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, val, true, arrayIndex, inst->instanceId, " (builtin, all-instances object write)");
#endif
                    }
                    Runner_popInstanceSnapshot(runner, snapBase);
                } else {
                    // Instance ID reference
                    Instance* target = VM_findInstanceByTarget(ctx, instanceType);
                    if (target != nullptr) {
                        VMBuiltins_setVariable(ctx, target, varDef->builtinVarId, varDef->name, val, arrayIndex);
#ifdef ENABLE_VM_TRACING
                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, target), "self", varDef->name, val, true, arrayIndex, target->instanceId, " (builtin)");
#endif
                    }
                }
            } else {
                Instance* targetInstance = instanceType == INSTANCE_OTHER && ctx->otherInstance != nullptr ? ctx->otherInstance : ctx->currentInstance;
                VMBuiltins_setVariable(ctx, targetInstance, varDef->builtinVarId, varDef->name, val, arrayIndex);
#ifdef ENABLE_VM_TRACING
                VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, targetInstance), "self", varDef->name, val, true, arrayIndex, targetInstance->instanceId, " (builtin)");
#endif
            }
        } else {
            // Resolve slot for this scope: VM_arraySetWithCoW handles CoW + materialisation + grow.
            RValue* slot = nullptr;
            switch (instanceType) {
                case INSTANCE_LOCAL: {
                    uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                    require(ctx->localVarCount > localSlot);
                    slot = &ctx->localVars[localSlot];
                    break;
                }
                case INSTANCE_SELF:
                case INSTANCE_GLOBAL:
                default: {
                    struct Instance* inst = (struct Instance*) ctx->currentInstance;
                    if (instanceType >= 0) {
                        inst = VM_findInstanceByTarget(ctx, instanceType);
                        if (inst == nullptr) {
                            const char* varTypeName = varTypeToString(varType);
                            char* valAsString = RValue_toString(val, ctx->runner->dataWin);
                            if (instanceType < INSTANCE_ID_BASE && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                                logWarn("VM: [%s] WRITE array var '%s[%d]' on object index %d (%s) but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, ctx->dataWin->objt.objects[instanceType].name, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            } else {
                                logWarn("VM: [%s] WRITE array var '%s[%d]' on instance %d but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            }
                            free(valAsString);
                            RValue_free(&val);
                            return;
                        }
                    } else if (instanceType == INSTANCE_OTHER && ctx->otherInstance != nullptr) {
                        inst = (Instance*) ctx->otherInstance;
                    } else if (instanceType == INSTANCE_GLOBAL) {
                        inst = ctx->globalScopeInstance;
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
                VM_arraySetWithCoW(ctx, slot, arrayIndex, val);
#ifdef ENABLE_VM_TRACING
                bool isSelfScope = (instanceType != INSTANCE_LOCAL && instanceType != INSTANCE_GLOBAL);
                const char* scopeName = instanceType == INSTANCE_LOCAL ? "local" : instanceType == INSTANCE_GLOBAL ? "global" : "self";
                VM_checkIfVariableShouldBeTracedAndLog(ctx, scopeName, isSelfScope ? nullptr : "self", varDef->name, val, true, arrayIndex, -1, "");
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

NOINLINE
static void handleAddString(VMContext* ctx, RValue a, RValue b, uint8_t resultType) {
    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        // String concatenation
        const char* sa = a.string != nullptr ? a.string : "";
        const char* sb = b.string != nullptr ? b.string : "";
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = (char *)safeMalloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeOwnedString(result), resultType);
    } else {
        // For anything else, we'll convert to numbers and then sum
#ifndef NO_RVALUE_INT64
        if (a.type == RVALUE_INT64 || b.type == RVALUE_INT64) {
            int64_t result = (a.type == RVALUE_STRING) ? (int64_t) GMLReal_strtod(a.string, nullptr) : a.int64;
            result += (b.type == RVALUE_STRING) ? (int64_t) GMLReal_strtod(b.string, nullptr) : b.int64;
            RValue_free(&a);
            RValue_free(&b);
            stackPushTyped(ctx, RValue_makeInt64(result), resultType);
            return;
        }
#endif
        if (a.type == RVALUE_INT32 || b.type == RVALUE_INT32) {
            int32_t result = (a.type == RVALUE_STRING) ? (int32_t) GMLReal_strtod(a.string, nullptr) : a.int32;
            result += (b.type == RVALUE_STRING) ? (int32_t) GMLReal_strtod(b.string, nullptr) : b.int32;
            RValue_free(&a);
            RValue_free(&b);
            stackPushTyped(ctx, RValue_makeInt32(result), resultType);
            return;
        }
        GMLReal result = RValue_toReal(a) + RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPushTyped(ctx, RValue_makeReal(result), resultType);
    }
}

NOINLINE
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
        char* result = (char *)safeMalloc(len * count + 1);
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
    uint8_t type1 = instrType1(instr);
    uint8_t type2 = instrType2(instr);
    GMLReal divisor = RValue_toReal(b);
    // In GameMaker's native runner, ONLY integer/integer division throws a hard error on zero, float/variable types rely on IEEE 754 (produces NaN)
    if ((type1 == GML_TYPE_INT32 || type1 == GML_TYPE_INT64) && (type2 == GML_TYPE_INT32 || type2 == GML_TYPE_INT64)) {
        requireMessageFormatted(__FILE__, __LINE__, divisor != 0.0, "VM: [%s] DoDiv :: Divide by zero", ctx->currentCodeName);
    }
    GMLReal result = RValue_toReal(a) / divisor;
    RValue_free(&a);
    RValue_free(&b);
    stackPushTyped(ctx, RValue_makeReal(result), instrType2(instr));
}

static void handleRem(VMContext* ctx, uint32_t instr) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    int64_t divisor = RValue_toInt64(b);
    requireMessageFormatted(__FILE__, __LINE__, divisor != 0, "VM: [%s] DoRem :: Divide by zero", ctx->currentCodeName);
    int64_t result = RValue_toInt64(a) / divisor;
    RValue_free(&a);
    RValue_free(&b);
    stackPushTyped(ctx, RValue_makeInt64(result), instrType2(instr));
}

static void handleMod(VMContext* ctx, uint32_t instr) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    GMLReal divisor = RValue_toReal(b);
    requireMessageFormatted(__FILE__, __LINE__, divisor != 0.0, "VM: [%s] DoMod :: Divide by zero", ctx->currentCodeName);
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
        case 0x60: { char* s = RValue_toString(val, ctx->runner->dataWin); result = RValue_makeOwnedString(s); break; }
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
        case 0x62: { char* s = RValue_toString(val, ctx->runner->dataWin); result = RValue_makeOwnedString(s); break; }
        case 0xF2: result = val; break;

#ifndef NO_RVALUE_INT64
        // Int64 (3) -> other
        case 0x03: result = RValue_makeReal((GMLReal) val.int64); break;
        case 0x23: result = RValue_makeInt32((int32_t) val.int64); break;
        case 0x43: result = RValue_makeBool(val.int64 > 0); break;
        case 0x53: result = val; break; // Int64 -> Variable (passthrough)
#elif IS_WAD17_OR_HIGHER_ENABLED
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
        case 0x64: { char* s = RValue_toString(val, ctx->runner->dataWin); result = RValue_makeOwnedString(s); break; }

        // Variable (5) -> other
        case 0x05: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x15: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x25: result = RValue_makeInt32(RValue_toInt32(val)); break;
        case 0x35: result = RValue_makeInt64(RValue_toInt64(val)); break;
        case 0x45: result = RValue_makeBool(RValue_toBool(val)); break;
        case 0x55: result = val; break; // Variable -> Variable (identity)
        case 0x65: { char* s = RValue_toString(val, ctx->runner->dataWin); result = RValue_makeOwnedString(s); break; }
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
            logWarn("VM: [%s] Conv unhandled conversion 0x%02X (src=0x%X dst=0x%X)\n", ctx->currentCodeName, convKey, srcType, dstType);
            result = val;
            break;
    }

    // Don't free the old value if we're returning the same value (identity conversion or passthrough)
    if (result.string != val.string || result.type != val.type) {
        RValue_free(&val);
    }

    // Set gmlStackType to the bytecode-declared destination type so Dup and other byte-level walks see the correct native footprint.
    result.gmlStackType = dstType;
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
#if IS_WAD17_OR_HIGHER_ENABLED
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

// Converts a native byte count to RValue slot count by walking the stack backwards from a given position.
// Reads each slot's gmlStackType (set at push time) to compute its native footprint.
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

static void handleDup(VMContext* ctx, uint32_t instr) {
    uint16_t operand = (uint16_t)(instr & 0xFFFF);
    uint8_t type1 = instrType1(instr);
    int32_t typeSize = gmlTypeNativeSize(type1);

#if IS_WAD17_OR_HIGHER_ENABLED
    // Swap mode (WAD17+): bit 15 of operand is set.
    // The Dup instruction doubles as a stack rotation when bit 15 is set.
    // It takes the top N items and moves them below the next M items.
    // Bits 0-10: top group size (in native type units)
    // Bits 11-14: bottom group size (in native type units)
    if (IS_WAD17_OR_HIGHER(ctx) && (operand & 0x8000) != 0) {
        int32_t topNativeCount = operand & 0x7FF;
        int32_t bottomNativeCount = (operand >> 11) & 0xF;

        int32_t topSlots = topNativeCount;
        int32_t bottomSlots = bottomNativeCount;

        if (type1 != GML_TYPE_VARIABLE) {
            int32_t topBytes = topNativeCount * typeSize;
            int32_t bottomBytes = bottomNativeCount * typeSize;
            topSlots = bytesToSlotCount(ctx, topBytes, ctx->stack.top);
            bottomSlots = bytesToSlotCount(ctx, bottomBytes, ctx->stack.top - topSlots);
        }

        int32_t totalSlots = topSlots + bottomSlots;
        int32_t baseIdx = ctx->stack.top - totalSlots;

        // Save top group to temp
        RValue temp[32];
        for (int32_t i = 0; topSlots > i; i++) {
            temp[i] = ctx->stack.slots[ctx->stack.top - topSlots + i];
        }

        // Shift bottom group up to where top group was
        {
        for (int32_t i = bottomSlots - 1; i >= 0; i--) {
            ctx->stack.slots[baseIdx + topSlots + i] = ctx->stack.slots[baseIdx + i];
        }
        }

        // Place top group at the bottom
        {
        for (int32_t i = 0; topSlots > i; i++) {
            ctx->stack.slots[baseIdx + i] = temp[i];
        }
        }
        return;
    }
#endif

    // Normal dup mode: total bytes to duplicate = (operand + 1) * sizeof(type1)
    // WAD17+ uses the low 15 bits of operand.
    // WAD16 and below uses the low 15 bits of operand.
    // Bit 15 is the swap-mode flag, handled above.
    int32_t operandCount;
#if IS_WAD17_OR_HIGHER_ENABLED
    operandCount = IS_WAD17_OR_HIGHER(ctx) ? (int32_t)(operand & 0x7FFF) : (int32_t)(operand & 0xFF);
#else
    operandCount = (int32_t)(operand & 0xFF);
#endif
    int32_t totalBytes = (operandCount + 1) * typeSize;
    int32_t count = bytesToSlotCount(ctx, totalBytes, ctx->stack.top);

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
#if IS_WAD17_OR_HIGHER_ENABLED
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
    RValue* args = nullptr;
	RValue stackArgs[VM_MAX_STACK_ARGS] = {0};
	bool useStackArgs = argCount <= VM_MAX_STACK_ARGS;
    if (argCount > 0) {
        args = useStackArgs ? stackArgs : (RValue *)safeCalloc(argCount, sizeof(RValue));
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
            char* display = RValue_toStringFancy(args[i], ctx->runner->dataWin);

            if (i > 0) {
                size_t bufsz = strlen(functionArgumentList) + 2 + strlen(display) + 1;
                char* tmp = (char *)safeMalloc(bufsz);
                snprintf(tmp, bufsz, "%s, %s", functionArgumentList, display);
                free(functionArgumentList);
                functionArgumentList = tmp;
            } else {
                free(functionArgumentList);
                functionArgumentList = safeStrdup(display);
            }
            free(display);
        }

        logInfo("VM: [%s] Calling function \"%s(%s)\"\n", ctx->currentCodeName, funcName, functionArgumentList);
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
            if (!useStackArgs) free(args);
        }

#ifdef ENABLE_VM_TRACING
        if (functionIsBeingTraced) {
            char* returnValueAsString = RValue_toStringFancy(result, ctx->runner->dataWin);
            logInfo("VM: [%s] Built-in function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
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
            char* returnValueAsString = RValue_toStringFancy(result, ctx->runner->dataWin);
            logInfo("VM: [%s] Script function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
            free(returnValueAsString);
            free(functionArgumentList);
        }
#endif

        // Free arguments (VM_callCodeIndex copies what it needs)
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            if (!useStackArgs) free(args);
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
        logWarn("VM: [%s] Unknown function \"%s\"!\n", callerName, unknownFuncName);
    } else {
        free(dedupKey);
    }
#endif

    // Free arguments and push undefined
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        if (!useStackArgs) free(args);
    }

#ifdef ENABLE_VM_TRACING
    if (functionIsBeingTraced) {
        free(functionArgumentList);
    }
#endif

    stackPush(ctx, RValue_makeUndefined());
}

#if IS_WAD17_OR_HIGHER_ENABLED
// BC17+ CALLV: dynamic call through a variable (method/script reference).
// Stack layout (top -> bottom): function, instance, arg[N-1], ..., arg[0]
// argCount is in the low 16 bits of the instruction.
static void handleCallV(VMContext* ctx, uint32_t instr) {
    int32_t argCount = instr & 0xFFFF;

    RValue function = stackPop(ctx);
    RValue instance = stackPop(ctx);

    RValue* args = nullptr;
	RValue stackArgs[VM_MAX_STACK_ARGS] = {0};
	bool useStackArgs = argCount <= VM_MAX_STACK_ARGS;
    if (argCount > 0) {
        args = useStackArgs ? stackArgs : (RValue *)safeCalloc(argCount, sizeof(RValue));
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
    } else if (DataWin_isVersionAtLeast(ctx->dataWin, 2, 3, 0, 0) && (function.type == RVALUE_INT32 || function.type == RVALUE_INT64 || function.type == RVALUE_REAL || function.type == RVALUE_BOOL)) {
        // In GMS 2.3.0+: CALLV coerces any numeric operand to a script index and wraps it with method() before calling.
        // Example: A button callback field assigned "field = some_script" without method().
        int32_t rawArg = RValue_toInt32(function);
        if (rawArg >= 0 && ctx->dataWin->func.functionCount > (uint32_t) rawArg) {
            const char* funcName = ctx->dataWin->func.functions[rawArg].name;
            if (funcName != nullptr) {
                ptrdiff_t idx = shgeti(ctx->codeIndexByName, (char*) funcName);
                if (idx >= 0) {
                    codeIndex = ctx->codeIndexByName[idx].value;
                } else {
                    ptrdiff_t bidx = shgeti(ctx->builtinMap, (char*) funcName);
                    if (bidx >= 0) builtin = ctx->builtinMap[bidx].value;
                }
            }
        }
    }

    // Decide target self: prefer method's bound instance, else the stack-provided instance.
    int32_t targetInstance = (boundInstance > 0) ? boundInstance : RValue_toInt32(instance);
    Instance* savedSelf = ctx->currentInstance;
    if (targetInstance != INSTANCE_SELF && targetInstance != 0) {
        Instance* target = VM_findInstanceByTarget(ctx, targetInstance);
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
            logWarn("VM: [%s] Unknown function \"%s\"! (via CallV)\n", callerName, unresolvedName);
        } else {
            free(dedupKey);
        }
#endif
        result = RValue_makeUndefined();
    } else {
        logWarn("VM: [%s] CALLV with unresolvable function reference (type=%d, codeIndex=%d)\n", ctx->currentCodeName, function.type, codeIndex);
#ifdef ENABLE_WAD17
        VMException* exception = (VMException *)safeCalloc(1, sizeof(VMException));
        exception->message = safeStrdup("CALLV with unresolvable function reference");
        ctx->exception = exception;
        result = RValue_makeUndefined();
#endif
    }

    ctx->currentInstance = savedSelf;

    RValue_free(&function);
    RValue_free(&instance);
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        if (!useStackArgs) free(args);
    }

    stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
}
#endif

// ===[ With-Statement Helpers (PushEnv/PopEnv) ]===

// Resolves a collision/instance "target" argument by mapping the special INSTANCE_SELF and INSTANCE_OTHER to the concrete instance ID they refer to.
int32_t VM_resolveInstanceTarget(VMContext* ctx, int32_t target) {
    if (target == INSTANCE_SELF) {
        Instance* self = (Instance*) ctx->currentInstance;
        return self != nullptr ? (int32_t) self->instanceId : INSTANCE_NOONE;
    }
    if (target == INSTANCE_OTHER) {
        Instance* other = (Instance*) ctx->otherInstance;
        return other != nullptr ? (int32_t) other->instanceId : INSTANCE_NOONE;
    }
    return target;
}

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
    if (IS_WAD17_OR_HIGHER(ctx) && target == INSTANCE_STACKTOP) {
        target = resolveInstanceStackTop(ctx);
    }

    // Create env frame, save current context
    EnvFrame* frame = (EnvFrame *)safeMalloc(sizeof(EnvFrame));
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
        } else if (frame->savedOtherInstance != nullptr) {
            // No parent env frame, but we have an otherInstance (example: from collision events), so let's use it!
            switchToInstance(ctx, frame->savedOtherInstance);
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

    if (target >= 0 && INSTANCE_ID_BASE > target) {
        // Object index - copy the descendant-inclusive list for this object into the frame's own list. frame->instanceList has with-block lifetime (not the snapshot arena's loop lifetime), so we don't use the forEach macro; we just copy directly and filter "active" to match prior semantics (deactivated instances are skipped).
        if (ctx->dataWin->objt.count > (uint32_t) target) {
            Instance** source = runner->instancesByObject[target];
            int32_t sourceCount = (int32_t) arrlen(source);

            Instance** activeInstances = nullptr;
            repeat(sourceCount, i) {
                if (source[i]->active) {
                    arrput(activeInstances, source[i]);
                }
            }

            int32_t activeSourceCount = arrlen(activeInstances);

            // You may be thinking "wow this looks extremely dumb", well, THAT'S HOW GAMEMAKER HANDLES IT FOR SOME REASON
            // GameMaker is *quirky* like that
            if (activeSourceCount == 1) {
                arrput(frame->instanceList, activeInstances[0]);
            } else if (activeSourceCount == 2) {
                // Iterate in forward order
                arrput(frame->instanceList, activeInstances[0]);
                arrput(frame->instanceList, activeInstances[1]);
            } else {
                // Iterate in reverse order
                for (int32_t i = activeSourceCount - 1; i >= 0; i--) {
                    Instance* inst = activeInstances[i];
                    arrput(frame->instanceList, inst);
                }
            }

            arrfree(activeInstances);
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

    if (target >= INSTANCE_ID_BASE) {
        // Instance ID - find specific instance
        Instance* inst = hmget(runner->instancesById, target);
        if (inst != nullptr && inst->active) {
            switchToInstance(ctx, inst);
            return;
        }

        // Instance not found, skip the block
        ctx->ip = instrAddr + jumpOffset;
        return;
    }

    if (0 > target) {
        // This is triggered in DELTARUNE Chapter 5 in room_dw_garden_meetflowery. This is not a bug, as
        // gml_Object_obj_bush_leaf_Step_0 calls scr_depth(-20). scr_depth then uses with(arg0), causing this log.
        logWarn("VM: [%s] PushEnv with negative target %d, this could be a Int64 number that is getting truncated to Int32!\n", ctx->currentCodeName, target);
    } else {
        logWarn("VM: [%s] PushEnv with unhandled target %d\n", ctx->currentCodeName, target);
    }
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
        case RVALUE_ASSETREF:  return "ASSETREF";
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
        case BREAK_ISNULLISH:   return "isnullish";
        case BREAK_PUSHREF:     return "pushref";
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
    {
    for (int i = 1; entryCount > i; i++) {
        CountEntry tmp = entries[i];
        int j = i;
        while (j > 0 && entries[j - 1].count < tmp.count) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }
    }

    logInfo("=== Opcode Profiler Report ===\n");
    logInfo("Total instructions executed: %llu\n", (unsigned longlong) total);
    logInfo("%-12s %-6s %16s %8s\n", "Opcode", "Hex", "Count", "Pct");
    {
    forEachIndexed(CountEntry, entry, i, entries, entryCount) {
        (void) i;
        double pct = total > 0 ? (100.0 * (double)(int64_t)entry->count / (double)(int64_t)total) : 0.0;
        logInfo("%-12s 0x%02X   %16llu %7.2f%%\n", opcodeName((uint8_t) entry->key), (uint8_t) entry->key, (unsigned longlong) entry->count, pct);
    }
    }

    // Per-opcode breakdown by type variant. Sorted within each opcode by count desc.
    logInfo("\n--- Type variant breakdown (per opcode) ---\n");
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

        logInfo("%s (0x%02X): %llu total\n", opcodeName(opcode), opcode, (unsigned longlong) entry->count);
        forEachIndexed(CountEntry, ve, vi, variantEntries, variantCount) {
            (void) vi;
            uint8_t type1 = (uint8_t) ((ve->key >> 4) & 0xF);
            uint8_t type2 = (uint8_t) (ve->key & 0xF);
            double vpct = entry->count > 0 ? (100.0 * (double)(int64_t)ve->count / (double)(int64_t)entry->count) : 0.0;
            logInfo("    .%c.%c  %16llu %7.2f%%\n", gmlTypeChar(type1), gmlTypeChar(type2), (unsigned longlong) ve->count, vpct);
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
                logInfo("    -- runtime types (a, b):\n");
                forEachIndexed(CountEntry, re, ri, rvEntries, rvCount) {
                    (void) ri;
                    uint8_t typeA = (uint8_t) ((re->key >> 4) & 0xF);
                    uint8_t typeB = (uint8_t) (re->key & 0xF);
                    double rpct = rvTotal > 0 ? (100.0 * (double)(int64_t)re->count / (double)(int64_t)rvTotal) : 0.0;
                    logInfo("    (%-6s, %-6s) %16llu %7.2f%%\n", rvalueTypeName(typeA), rvalueTypeName(typeB), (unsigned longlong) re->count, rpct);
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
            {
            for (int i = 1; breakCount > i; i++) {
                CountEntry tmp = breakEntries[i];
                int j = i;
                while (j > 0 && breakEntries[j - 1].count < tmp.count) {
                    breakEntries[j] = breakEntries[j - 1];
                    j--;
                }
                breakEntries[j] = tmp;
            }
            }
            logInfo("    -- sub-opcodes:\n");
            forEachIndexed(CountEntry, be, bi, breakEntries, breakCount) {
                (void) bi;
                int16_t breakType = (int16_t) -((int) be->key);
                double bpct = entry->count > 0 ? (100.0 * (double)(int64_t)be->count / (double)(int64_t)entry->count) : 0.0;
                logInfo("    %-12s (%4d) %16llu %7.2f%%\n", breakSubOpName(breakType), (int) breakType, (unsigned longlong) be->count, bpct);
            }
        }
    }
    logInfo("==============================\n");
}
#endif // ENABLE_VM_OPCODE_PROFILER

// Forward declaration for formatInstruction (defined in disassembler section, used by trace-opcodes)
static void formatInstruction(VMContext* ctx, const uint8_t* bytecodeBase, uint32_t instrAddr, uint32_t instr, const uint8_t* extraData, char* opcodeStr, size_t opcodeSize, char* operandStr, size_t operandSize, char* commentStr, size_t commentSize);

#if IS_WAD17_OR_HIGHER_ENABLED
// ===[ BREAK sub-opcode handlers (BC17+) ]===

static void handleBreakChkIndex(VMContext* ctx, uint32_t instrAddr) {
    // Validate top-of-stack array index is in [0, 32000)
    RValue* top = stackPeek(ctx);
    int32_t idx = RValue_toInt32(*top);
    if (0 > idx || 32000 <= idx) {
        logError("VM: chkindex out of bounds: %d at offset %u in %s\n", idx, instrAddr, ctx->currentCodeName);
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
        result = RValue_makeUndefined();
    }
    stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
    RValue_free(&arrayRef);
}

static void handleBreakPopAF(VMContext* ctx) {
    // Pop index + array ref + value, store value at array[index].
    // CoW via VM_arraySetWithCoW requires a slot pointer, since the stack-held arrayRef is a weak view, the real slot is whatever variable holds this array.
    // We can't easily recover the slot here, so we write directly into the array (no CoW fork at this level, fork already happened when the top-level variable was first written, or on a PUSHAC materialisation).
    // Assert the array is uniquely-owned or matches the current scope owner. A mismatch here means a shared/aliased array is about to be mutated in place, which silently breaks CoW semantics. BC17+ default mode (pass by reference) is expected to satisfy this since fork already happened at the top-level write. If this fires, a CoW path upstream failed to fork.
    int32_t idx = stackPopInt32(ctx);
    RValue arrayRef = stackPop(ctx);
    RValue value = stackPop(ctx);
    if (arrayRef.type == RVALUE_ARRAY && arrayRef.array != nullptr && idx >= 0) {
        GMLArray* arr = arrayRef.array;
        requireMessageFormatted(__FILE__, __LINE__, arr->refCount == 1 || arr->owner == ctx->currentArrayOwner, "BREAK_POPAF: Writing through shared/aliased array without prior CoW fork in %s: arr=%p refCount=%d owner=%p currentArrayOwner=%p idx=%d", ctx->currentCodeName, (void*) arr, arr->refCount, arr->owner, ctx->currentArrayOwner, idx);
        GMLArray_set(arr, idx, value);
    }
    RValue_free(&arrayRef);
    RValue_free(&value);
}

static void handleBreakPushAC(VMContext* ctx, uint32_t instrAddr) {
    // Pop index + parent array ref, push sub-array at parent[index]. Materialise a fresh sub-array if the slot isn't already an RVALUE_ARRAY (multi-dim auto-init).
    int32_t idx = stackPopInt32(ctx);
    RValue arrayRef = stackPop(ctx);
    if (arrayRef.type != RVALUE_ARRAY || arrayRef.array == nullptr) {
        logError("VM: pushac on non-array (type=%d) at offset %u in %s\n", arrayRef.type, instrAddr, ctx->currentCodeName);
        abort();
    }
    GMLArray* parent = arrayRef.array;
    GMLArray_growTo(parent, idx + 1);
    RValue* parentSlot = GMLArray_slot(parent, idx);
    if (parentSlot->type != RVALUE_ARRAY || parentSlot->array == nullptr) {
        RValue_free(parentSlot);
        GMLArray* sub = GMLArray_create(ctx->dataWin, 0);
        sub->owner = ctx->currentArrayOwner;
        *parentSlot = RValue_makeArray(sub);
    } else {
        cowForkSlotForModificationIfNeeded(ctx, parentSlot);
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

static void handleBreakIsNullish(VMContext* ctx) {
    // Peek the top of the stack and push a bool above it: true if the value is "nullish"
    RValue* value = stackPeek(ctx);
    // TODO: We need to support a RValue pointer_null later, because that's also considered as "nullish" here!
    bool nullish = value->type == RVALUE_UNDEFINED;
    stackPush(ctx, RValue_makeBool(nullish));
}

static void handleBreakPushRef(VMContext* ctx, const uint8_t* extraData) {
    // Push an asset reference encoded in the 32-bit operand: high byte = asset type, low 24 bits = index.
    // If it is a script reference, the low 24 bits is a funcIdx which we resolve to a callable method; everything else is a plain asset.
    uint32_t operand = BinaryUtils_readUint32Aligned(extraData);
    uint8_t assetType = (uint8_t) ((operand >> 24) & 0xFF);
    int32_t index = (int32_t) (operand & 0x00FFFFFF);

    if (assetType == ASSET_TYPE_SCRIPT) {
        // Resolve to a callable method
        if (ctx->dataWin->func.functionCount > (uint32_t) index) {
            FuncCallCache* cache = &ctx->funcCallCache[index];
            if (cache->scriptCodeIndex >= 0) {
                stackPushTyped(ctx, RValue_makeMethodFromCodeIndexAndInstanceId(cache->scriptCodeIndex, -1), GML_TYPE_VARIABLE);
                return;
            }
            GMLMethod* method;
            if (cache->builtin != nullptr) {
                method = GMLMethod_createBuiltin((BuiltinFunc) cache->builtin, -1);
            } else {
                method = GMLMethod_createUnresolved(ctx->dataWin->func.functions[index].name, -1);
            }

            stackPushTyped(ctx, RValue_makeMethod(method), GML_TYPE_VARIABLE);
        } else {
            stackPushTyped(ctx, RValue_makeUndefined(), GML_TYPE_VARIABLE);
        }
        return;
    }

    stackPushTyped(ctx, RValue_makeAssetRef(index, assetType), GML_TYPE_VARIABLE);
}

static void handleBreak(VMContext* ctx, uint32_t instr, uint32_t instrAddr, const uint8_t* extraData) {
    if (IS_WAD16_OR_BELOW(ctx)) return;
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
        case BREAK_ISNULLISH:   handleBreakIsNullish(ctx); break;
        case BREAK_PUSHREF:     handleBreakPushRef(ctx, extraData); break;
        default:
            logError("VM: Unknown BREAK sub-opcode %d at offset %u in %s\n", breakType, instrAddr, ctx->currentCodeName);
            abort();
    }
}
#endif

#define VM_SYNC_IP()    do { ctx->ip = ip; } while (0)
#define VM_RELOAD_IP()  do { ip = ctx->ip; } while (0)

// Gets the value a code block returns when it ends without an explicit "return <value>"
static inline RValue scriptFallthroughReturnValue(VMContext* ctx) {
    if (DataWin_isVersionAtLeast(ctx->dataWin, 2, 3, 1, 0)) {
        return RValue_makeUndefined();
    }
    return RValue_makeReal(0.0);
}

// Unwinds and frees the VMStack down to the newStackTop
static void unwindVMStack(VMContext* ctx, int32_t newStackTop) {
    repeat(ctx->stack.top - newStackTop, i) {
        RValue_free(&ctx->stack.slots[i + newStackTop]);
    }
    ctx->stack.top = newStackTop;
}

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
#ifdef ENABLE_WAD17
        if (ctx->exception != nullptr) {
#ifdef ENABLE_VM_EXCEPTIONS_LOGS
            logError("VM: Exception thrown! Stack Top is %d\n", ctx->exceptionHandlerStackTop);
#endif
            if (ctx->exceptionHandlerStackTop == 0) {
                // TODO: When Butterscotch is better, we could have a strict mode that DOES throw a error
                logError("VM: The exception handler frame stack is 0, but we have a pending exception to be dispatched! This would've technically crashed the game in the original runner... or we aren't handling exceptions correctly. We'll swallow the exception and hope for the best... (Exception: %s)\n", ctx->exception->message);
                free(ctx->exception->message);
                free(ctx->exception);
                ctx->exception = nullptr;
                return RValue_makeUndefined();
            }

            // Restore caller frame
            ExceptionHandlerFrame* exceptionHandlerFrame = &ctx->exceptionHandlerFrameStack[ctx->exceptionHandlerStackTop - 1];

            // Not for us, propagate the exception!
            if (exceptionHandlerFrame->boundToCallDepth != ctx->callDepth) {
#ifdef ENABLE_VM_EXCEPTIONS_LOGS
                logWarn("VM: We wanted %d but we are %d - Propagating...\n", exceptionHandlerFrame->boundToCallDepth, ctx->callDepth);
#endif
                return RValue_makeUndefined();
            }

            // We DO NOT want to pop the handler, it will be popped by @@try_unhook@@
            bool isException = exceptionHandlerFrame->jumpToOnException != -1;
            ip = isException ? exceptionHandlerFrame->jumpToOnException : exceptionHandlerFrame->jumpToOnSuccess;
            unwindVMStack(ctx, exceptionHandlerFrame->stackTop);

            VM_SYNC_IP();

#ifdef ENABLE_VM_EXCEPTIONS_LOGS
            logWarn("VM: Jumped to %d due to exception handler, is this a exception? %d\n", ip, isException);
#endif

            if (isException) {
                // On a exception, GameMaker pops the struct and stores it in the variable of the "catch (_nameOfTheExceptionHere) {"
                Instance* gmlStruct = Runner_createStruct(ctx->runner);
                // TODO: longMessage/script/stacktrace
                VM_structSet(ctx, gmlStruct, "message", RValue_makeOwnedString(safeStrdup(ctx->exception->message)), -1);
                stackPush(ctx, RValue_makeStructAndIncRef(gmlStruct));
                free(ctx->exception->message);
                free(ctx->exception);
            } else {
                // Will be unparked by finish_finally
                ctx->parkedException = ctx->exception;
            }

            ctx->exception = nullptr;
        }
#endif

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
                    logInfo("VM: [%s] @%04X (%d) [0x%08X] %s %s [stack=%d] %s\n", ctx->currentCodeName, instrAddr, instrAddr, instr, opcodeStr, operandStr, ctx->stack.top, stackBuf);
                } else {
                    logInfo("VM: [%s] @%04X (%d) [0x%08X] %s [stack=%d] %s\n", ctx->currentCodeName, instrAddr, instrAddr, instr, opcodeStr, ctx->stack.top, stackBuf);
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
#ifdef ENABLE_VM_TRACING
                                switch (instanceType) {
                                    case INSTANCE_SELF: {
                                        Instance* inst = (Instance*) ctx->currentInstance;
                                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, val, false, -1, inst->instanceId, "");
                                        break;
                                    }
                                    case INSTANCE_LOCAL:
                                        VM_checkIfVariableShouldBeTracedAndLog(ctx, "local", nullptr, varDef->name, val, false, -1, -1, "");
                                        break;
                                    case INSTANCE_GLOBAL:
                                        VM_checkIfVariableShouldBeTracedAndLog(ctx, "global", nullptr, varDef->name, val, false, -1, -1, "");
                                        break;
                                    case INSTANCE_OTHER: {
                                        Instance* inst = (Instance*) ctx->otherInstance;
                                        VM_checkIfVariableShouldBeTracedAndLog(ctx, instanceObjectName(ctx, inst), "self", varDef->name, val, false, -1, inst->instanceId, "");
                                        break;
                                    }
                                }
#endif
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
#if IS_WAD17_OR_HIGHER_ENABLED
                uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
                if (varType == VARTYPE_ARRAYPUSHAF || varType == VARTYPE_ARRAYPOPAF) {
                    Variable* varDef = resolveVarDef(ctx, varRef);
                    uint32_t localSlot = resolveLocalSlot(ctx, varDef->varID);
                    require(ctx->localVarCount > localSlot);
                    pushTopLevelArrayRef(ctx, &ctx->localVars[localSlot], varType == VARTYPE_ARRAYPOPAF);
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
#ifdef ENABLE_VM_TRACING
                VM_checkIfVariableShouldBeTracedAndLog(ctx, "local", nullptr, varDef->name, val, false, -1, -1, "");
#endif
                break;
            }
            case OP_PUSHGLB: {
                uint32_t varRef = resolveVarOperand(extraData);
                // TODO: Re-add fast-path here!
                RValue val = resolveVariableRead(ctx, INSTANCE_GLOBAL, varRef);
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
                // BC17: VARTYPE_INSTANCE encodes (instanceId - INSTANCE_ID_BASE) in the instruction's lower 16 bits.
                if (varType == VARTYPE_INSTANCE) instanceType += INSTANCE_ID_BASE;
                int32_t type2 = instrType2(instr); // source type (what's on stack)
                if (type1 == GML_TYPE_VARIABLE && varType == VARTYPE_NORMAL) {
                    // Inline fast path for the simple variable-assignment case: type1==VARIABLE, which is ~99.998% of all Pops in real workloads
                    RValue val = stackPop(ctx);
                    val = coerceIntStoreToReal(val, type2);
                    resolveVariableWrite(ctx, instanceType, varRef, val);
                } else {
                    handlePop(ctx, type1, type2, varRef, varType, instanceType);
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
                    slotA->gmlStackType = instrType2(instr);
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
                    slotA->gmlStackType = instrType2(instr);
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
                    slotA->gmlStackType = instrType2(instr);
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
                        switch (top->type) {
                            case RVALUE_INT32:
                                top->int32 = top->int32 > 0 ? 1 : 0;
                                top->type = RVALUE_BOOL;
                                fastHit = true;
                                break;
                            case RVALUE_BOOL:
                                // Already 0/1; nothing to do
                                fastHit = true;
                                break;
                            case RVALUE_REAL:
                                top->int32 = top->real > (GMLReal) 0.5 ? 1 : 0;
                                top->type = RVALUE_BOOL;
                                fastHit = true;
                                break;
                        }
                        break;
                    case 0x25: // Variable -> Int32
                        switch (top->type) {
                            case RVALUE_INT32:
                                fastHit = true;
                                break;
                            case RVALUE_BOOL:
                                top->type = RVALUE_INT32;
                                fastHit = true;
                                break;
                            case RVALUE_REAL:
                                top->int32 = (int32_t) top->real;
                                top->type = RVALUE_INT32;
                                fastHit = true;
                                break;
                        }
                        break;
                    case 0x02: // Int32 -> Double (Real)
                        top->real = (GMLReal) top->int32;
                        top->type = RVALUE_REAL;
                        fastHit = true;
                        break;
                }

                if (fastHit) {
                    top->gmlStackType = dstType;
                } else {
                    handleConv(ctx, srcType, dstType, convKey);
                }
                break;
            }

            // Comparison
            case OP_CMP: {
                RValue* slotA = &ctx->stack.slots[ctx->stack.top - 2];
                RValue* slotB = &ctx->stack.slots[ctx->stack.top - 1];

                // Inline fast path for INT32/INT32
                if (slotA->type == RVALUE_INT32 && slotB->type == RVALUE_INT32) {
                    int32_t a = slotA->int32;
                    int32_t b = slotB->int32;
                    bool result;
                    switch (instrCmpKind(instr)) {
                        case CMP_LT:  result = b > a;  break;
                        case CMP_LTE: result = b >= a; break;
                        case CMP_EQ:  result = a == b; break;
                        case CMP_NEQ: result = a != b; break;
                        case CMP_GTE: result = a >= b; break;
                        case CMP_GT:  result = a > b;  break;
                        default:      result = false;  break;
                    }
                    slotA->int32 = result ? 1 : 0;
                    slotA->type = RVALUE_BOOL;
                    slotA->gmlStackType = GML_TYPE_BOOL;
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
#if IS_WAD17_OR_HIGHER_ENABLED
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
                return scriptFallthroughReturnValue(ctx);

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
#if IS_WAD17_OR_HIGHER_ENABLED
                handleBreak(ctx, instr, instrAddr, extraData);
#endif
                break;

            default:
                logError("VM: Unknown opcode 0x%02X at offset %u\n", opcode, instrAddr);
                abort();
        }
    }

    return scriptFallthroughReturnValue(ctx);
}

// Rewrites WAD 14 bytecode opcodes to use WAD 16 bytecode opcodes
static void rewriteBytecode14To16(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    uint8_t* buf = dw->bytecodeBuffer;
    size_t base = dw->bytecodeBufferBase;

    // Synthesize varIDs for each variable
    repeat(dw->vari.variableCount, i) {
        dw->vari.variables[i].varID = (int32_t) i;
    }

    repeat(dw->code.count, codeIdx) {
        CodeEntry* entry = &dw->code.entries[codeIdx];
        if (entry->length == 0) continue;

        uint32_t ip = entry->bytecodeAbsoluteOffset;
        uint32_t end = ip + entry->length;

        while (end > ip) {
            uint32_t instrAddr = ip;
            uint32_t instr = BinaryUtils_readUint32(&buf[instrAddr - base]);
            uint8_t oldKind = (instr >> 24) & 0xFF;
            uint8_t type1 = (instr >> 16) & 0x0F;

            uint8_t newKind = oldKind;
            switch (oldKind) {
                case 0x03: newKind = OP_CONV; break;
                case 0x04: newKind = OP_MUL; break;
                case 0x05: newKind = OP_DIV; break;
                case 0x06: newKind = OP_REM; break;
                case 0x07: newKind = OP_MOD; break;
                case 0x08: newKind = OP_ADD; break;
                case 0x09: newKind = OP_SUB; break;
                case 0x0A: newKind = OP_AND; break;
                case 0x0B: newKind = OP_OR; break;
                case 0x0C: newKind = OP_XOR; break;
                case 0x0D: newKind = OP_NEG; break;
                case 0x0E: newKind = OP_NOT; break;
                case 0x0F: newKind = OP_SHL; break;
                case 0x10: newKind = OP_SHR; break;
                case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
                    newKind = OP_CMP;
                    instr = (instr & 0xFF0000FF) | ((uint32_t)(oldKind - 0x10) << 8);
                    break;
                case 0x41: newKind = OP_POP; break;
                case 0x82: newKind = OP_DUP; break;
                case 0x9D: newKind = OP_RET; break;
                case 0x9E: newKind = OP_EXIT; break;
                case 0x9F: newKind = OP_POPZ; break;
                case 0xB7: newKind = OP_B; break;
                case 0xB8: newKind = OP_BT; break;
                case 0xB9: newKind = OP_BF; break;
                case 0xBB: newKind = OP_PUSHENV; break;
                case 0xBC: newKind = OP_POPENV; break;
                case 0xDA: newKind = OP_CALL; break;
                case 0xC0:
                    if (type1 == GML_TYPE_INT16) {
                        newKind = OP_PUSHI;
                    }
                    break;
            }

            if (newKind != oldKind || oldKind == 0x11 || oldKind == 0x12 || oldKind == 0x13 || oldKind == 0x14 || oldKind == 0x15 || oldKind == 0x16) {
                instr = (instr & 0x00FFFFFF) | ((uint32_t)newKind << 24);
                BinaryUtils_writeUint32(&buf[instrAddr - base], instr);
            }

            // BC13/14 encodes the globalvar flag as bit 30 of the operand.
            // We will strip the globalvar flag and replace it with a proper BC16+ INSTANCE_GLOBAL instanceType
            if ((newKind == OP_PUSH || newKind == OP_POP) && type1 == GML_TYPE_VARIABLE) {
                uint32_t operandAddr = instrAddr + 4;
                uint32_t operand = BinaryUtils_readUint32(&buf[operandAddr - base]);
                // 0x40000000u = "globalvar"
                if (operand & 0x40000000u) {
                    // Remove the globalvar state from the operand
                    operand &= ~0x40000000u;
                    BinaryUtils_writeUint32(&buf[operandAddr - base], operand);
                    // And replace it to set the instanceType to "INSTANCE_GLOBAL"
                    instr = (instr & 0xFFFF0000u) | (uint16_t) INSTANCE_GLOBAL;
                    BinaryUtils_writeUint32(&buf[instrAddr - base], instr);
                }
            }

            uint32_t size = 4;
            switch (newKind) {
                case OP_PUSH: case OP_PUSHLOC: case OP_PUSHGLB: case OP_PUSHBLTN:
                    if (type1 == GML_TYPE_INT16) size = 4;
                    else if (type1 == GML_TYPE_DOUBLE || type1 == GML_TYPE_INT64) size = 12;
                    else size = 8;
                    break;
                case OP_POP:
                    size = (type1 == GML_TYPE_INT16) ? 4 : 8;
                    break;
                case OP_CALL:
                    size = 8;
                    break;
                case OP_BREAK:
                    size = (type1 == GML_TYPE_INT32) ? 8 : 4;
                    break;
            }
            ip += size;
        }
    }
}

// ===[ Public API ]===

VMContext* VM_create(DataWin* dataWin) {
#ifdef PLATFORM_PS2
    // Place VMContext in scratchpad RAM
    requireMessage(16384 >= sizeof(VMContext), "VMContext exceeds PS2 scratchpad size (16 KB)");
    VMContext* ctx = (VMContext*) 0x70000000;
    memset(ctx, 0, sizeof(VMContext));
#else
    VMContext* ctx = (VMContext *)safeCalloc(1, sizeof(VMContext));
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

    if (IS_WAD14_OR_BELOW(ctx)) {
        rewriteBytecode14To16(ctx);
    }

    VMBuiltins_checkIfBuiltinVarTableIsSorted();

    // Pre-resolve built-in variable IDs (replaces runtime strcmp chains with O(1) switch dispatch)
    repeat(dataWin->vari.variableCount, i) {
        Variable* var = &dataWin->vari.variables[i];
        // varID == -6 is the BC16 built-in sentinel.
        // In BC17, argument variables have instanceType == -6 (Builtin) with varID >= 0, so we also check instanceType.
        if (IS_WAD15_OR_HIGHER(ctx) && (var->varID == VARIABLE_BUILTIN || var->instanceType == -6)) {
            var->builtinVarId = VMBuiltins_resolveBuiltinVarId(var->name);
        } else if (IS_WAD14_OR_BELOW(ctx)) {
            // BC13/14 has no -6 sentinel in the file. Detect builtins by name and switch the VARI entry to use the BC16 sentinel so downstream dispatch paths treat it as a builtin.
            int16_t builtinId = VMBuiltins_resolveBuiltinVarId(var->name);
            if (builtinId != BUILTIN_VAR_UNKNOWN) {
                var->varID = -6;
                var->builtinVarId = builtinId;
            } else {
                var->builtinVarId = BUILTIN_VAR_UNKNOWN;
            }
        } else {
            var->builtinVarId = BUILTIN_VAR_UNKNOWN;
        }
    }

    // Build reference lookup maps (file buffer stays read-only)
    patchReferenceOperands(ctx);

    // Scan VARI entries to find max varID for global scope
    // Built-in variables have varID == -6 (sentinel), skip those
    // BC13/BC14: no eager scan, the sparse globalVarsSlotMap grows on first touch via resolveGlobalSlot.
    uint32_t maxGlobalVarID = 0;
    if (!IS_WAD14_OR_BELOW(ctx)) {
        forEach(Variable, v, dataWin->vari.variables, dataWin->vari.variableCount) {
            if (0 > v->varID) continue;
            // In BC17 any varID can be used as a global variable
            if (IS_WAD17_OR_HIGHER(ctx) || v->instanceType == INSTANCE_GLOBAL) {
                if ((uint32_t) v->varID + 1 > maxGlobalVarID) maxGlobalVarID = (uint32_t) v->varID + 1;
            }
        }
    }

    ctx->currentCodeIndex = -1;

    // V17+ static initialization tracking
    if (dataWin->gen8.wadVersion >= 17) {
        ctx->staticInitialized = (bool *)safeCalloc(dataWin->code.count, sizeof(bool));
        ctx->staticStructs = (Instance **)safeCalloc(dataWin->code.count, sizeof(Instance*));
    } else {
        ctx->staticInitialized = nullptr;
        ctx->staticStructs = nullptr;
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

    // Build selfVarNameMap: varName -> varID for self/instance-scoped variables.
    ctx->varNameMap = nullptr;
    int32_t maxSelfVarID = 0;
    forEach(Variable, variable, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (variable->varID >= 0) {
            ptrdiff_t existing = shgeti(ctx->varNameMap, (char*) variable->name);
            if (0 > existing) {
                shput(ctx->varNameMap, (char*) safeStrdup(variable->name), variable->varID);
            }
            if (variable->varID > maxSelfVarID)
                maxSelfVarID = variable->varID;
        }
    }
    ctx->nextDynamicVarID = maxSelfVarID + 1;

    // Build funcName -> codeIndex hash map from SCPT chunk
    ctx->codeIndexByName = nullptr;
    forEach(Script, s, dataWin->scpt.scripts, dataWin->scpt.count) {
        if (s->name != nullptr && s->codeId >= 0) {
            if (dataWin->code.count > (uint32_t) s->codeId) {
                const char* codeName = dataWin->code.entries[s->codeId].name;
                // Map the full code entry name (e.g. "gml_Script_SCR_GAMESTART")
                shput(ctx->codeIndexByName, (char*) codeName, s->codeId);
                // Also map the bare script name (e.g. "SCR_GAMESTART")
                // since the FUNC chunk references use bare names in CALL instructions
                shput(ctx->codeIndexByName, (char*) s->name, s->codeId);
            }
        }
    }

    // Also map code entry names directly for non-script code (object events, room creation codes, etc.)
    {
    repeat(dataWin->code.count, i) {
        const char* codeName = dataWin->code.entries[i].name;
        ptrdiff_t existing = shgeti(ctx->codeIndexByName, (char*) codeName);
        if (0 > existing) {
            shput(ctx->codeIndexByName, (char*) codeName, (int32_t) i);
        }
    }
    }

    // Build codeName -> CodeLocals* hash map
    ctx->codeLocalsMap = nullptr;
    {
    repeat(dataWin->func.codeLocalsCount, i) {
        CodeLocals* cl = &dataWin->func.codeLocals[i];
        shput(ctx->codeLocalsMap, safeStrdup(cl->name), cl);
        // In WAD 17+, CodeLocals uses "gml_GlobalScript_" prefix but callable CODE entries use "gml_Script_", so we'll map the "gml_Script_" variant too
        if (dataWin->gen8.wadVersion >= 17) {
            if (strncmp(cl->name, "gml_GlobalScript_", 17) == 0) {
                char scriptName[512];
                snprintf(scriptName, sizeof(scriptName), "gml_Script_%s", cl->name + 17);
                shput(ctx->codeLocalsMap, safeStrdup(scriptName), cl);
            }
        }
    }
    }

    // BC13/BC14/BC17+: build per-CodeLocals varID -> slot hmap so resolveLocalSlot is O(1)
    // BC17+ NEEDS "code.count" size entries because YoYo Games in their infinite wisdom thought "what if... we just didn't include some local variables in the localVars map? heck, sometimes we can just NOT include any CodeLocals!"... fun!
    // BC13/BC14 NEEDS the per-code map because BC<=14 has no CodeLocals chunk at all, so slots are allocated on first reference.
    ctx->codeLocalsSlotMaps = nullptr;
    if (dataWin->gen8.wadVersion >= 17 || 14 >= dataWin->gen8.wadVersion) {
        ctx->codeLocalsSlotMaps = (IntIntHashMap *)safeCalloc(dataWin->code.count, sizeof(*ctx->codeLocalsSlotMaps));
    }

    // Register built-in functions
    VMBuiltins_registerAll(ctx);

    // Pre-resolve all FUNC entries to cached builtin pointers or script code indices.
    // This eliminates per-call string hash lookups in handleCall.
    ctx->funcCallCacheCount = dataWin->func.functionCount;
    ctx->funcCallCache = (FuncCallCache *)safeMalloc(dataWin->func.functionCount * sizeof(FuncCallCache));
    {
    repeat(dataWin->func.functionCount, i) {
        const char* name = dataWin->func.functions[i].name;
        BuiltinFunc builtin = VM_findBuiltin(ctx, name);
        ctx->funcCallCache[i].builtin = (void*) builtin;
        if (builtin != nullptr) {
            ctx->funcCallCache[i].scriptCodeIndex = -1;
        } else {
            ptrdiff_t mapIdx = shgeti(ctx->codeIndexByName, (char*) name);
            ctx->funcCallCache[i].scriptCodeIndex = (mapIdx >= 0) ? ctx->codeIndexByName[mapIdx].value : -1;
        }
    }
    }

    logInfo("VM: Initialized with %u functions mapped\n", (uint32_t) shlen(ctx->codeIndexByName));

    return ctx;
}

void VM_reset(VMContext* ctx) {
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

    if (ctx->dataWin->gen8.wadVersion >= 17) {
        free(ctx->staticInitialized);
        free(ctx->staticStructs);
        ctx->staticInitialized = (bool *)safeCalloc(ctx->dataWin->code.count, sizeof(bool));
        ctx->staticStructs = (Instance **)safeCalloc(ctx->dataWin->code.count, sizeof(Instance*));
    }

    // Create the instance used for "self" in GLOB scripts
    Instance_free(ctx->globalScopeInstance);
    ctx->globalScopeInstance = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0);

#ifdef ENABLE_WAD17
    if (ctx->parkedException != nullptr) {
        free(ctx->parkedException->message);
        free(ctx->parkedException);
    }
    if (ctx->exception != nullptr) {
        free(ctx->exception->message);
        free(ctx->exception);
    }
#endif

    logInfo("VM: Reset complete\n");
}

static CodeLocals* resolveCodeLocals(VMContext* ctx, const char* codeName) {
    return shget(ctx->codeLocalsMap, (char*) codeName);
}

// Sets the currentCodeLocalsSlotMap for BC13/BC14/BC17+ games.
static void setCurrentCodeLocalsSlotMap(VMContext* ctx) {
    if (IS_WAD17_OR_HIGHER(ctx) || IS_WAD14_OR_BELOW(ctx)) {
        ctx->currentCodeLocalsSlotMap = &ctx->codeLocalsSlotMaps[ctx->currentCodeIndex];
    }
}

static uint32_t computeLocalsCount(VMContext* ctx, CodeEntry* code) {
    if (IS_WAD15_OR_HIGHER(ctx) && IS_WAD16_OR_BELOW(ctx)) {
        return code->localsCount;
    } else {
        // BC13/BC14/BC17+ (GM:S 2.3+): we can't trust the CODE entry's localsCount field (BC13/BC14 has no CodeLocals chunk, BC17+ sometimes omits entries from it), so we will get our cached map
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
    RValue* localVars = (RValue *)safeCalloc(localsCount, sizeof(RValue));
    ctx->localVars = localVars;
    ctx->localVarCount = localsCount;

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
    free(ctx->localVars);
    ctx->localVars = nullptr;
    ctx->localVarCount = 0;

    // Reset all values in the stack (see issue #137)
    // Keep in mind that recent GameMaker versions do seem to emit Pop/Popz when exiting loops (example: when using a repeat + return) but older versions DO need it
    unwindVMStack(ctx, 0);

    ctx->stack.top = 0;

    return result;
}


RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    // Save current frame
    CallFrame frame = {0};
    frame.savedIP = ctx->ip;
    frame.savedCodeEnd = ctx->codeEnd;
    frame.savedBytecodeBase = ctx->bytecodeBase;
    frame.savedLocals = ctx->localVars;
    frame.savedLocalsCount = ctx->localVarCount;
    frame.savedCodeName = ctx->currentCodeName;
    frame.savedSavearefBalance = ctx->savearefBalance;
    frame.savedCodeLocalsSlotMap = ctx->currentCodeLocalsSlotMap;
    frame.savedScriptArgs = ctx->scriptArgs;
    frame.savedScriptArgCount = ctx->scriptArgCount;
    frame.savedCurrentCodeIndex = ctx->currentCodeIndex;
    frame.parent = ctx->callStack;
    ctx->callStack = &frame;
    ctx->callDepth++;

    int32_t storedStackTop = ctx->stack.top;

    // Set up callee
    ctx->bytecodeBase = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);
    ctx->ip = code->offset;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;
    ctx->currentCodeIndex = codeIndex;

    setCurrentCodeLocalsSlotMap(ctx);

    uint32_t localsCount = computeLocalsCount(ctx, code);
    RValue* localVars = (RValue *)safeCalloc(localsCount, sizeof(RValue));
    ctx->localVars = localVars;
    ctx->localVarCount = localsCount;

    // Store arguments in scriptArgs (mirrors GMS 1.4's global argument stack).
    // Callee takes an INDEPENDENT reference for strings (strdup) and arrays (incRef) so
    // the caller's original args remain valid and owner-tracked by the caller.
    RValue* scriptArgs = nullptr;
    if (argCount > 0 && args != nullptr) {
        scriptArgs = (RValue *)safeCalloc(argCount, sizeof(RValue));
        repeat(argCount, argIdx) {
            RValue argCopy = RValue_makeIndependent(args[argIdx]);
            scriptArgs[argIdx] = argCopy;
        }
    }
    ctx->scriptArgs = scriptArgs;
    ctx->scriptArgCount = argCount;

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
    result = RValue_stealOwnershipOrCopy(result);

    // Restore caller frame
    CallFrame* saved = ctx->callStack;
    ctx->ip = saved->savedIP;
    ctx->codeEnd = saved->savedCodeEnd;
    ctx->bytecodeBase = saved->savedBytecodeBase;

    // Free callee locals
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }

    free(ctx->localVars);

    // Free callee script args
    {
    repeat(ctx->scriptArgCount, i) {
        RValue_free(&ctx->scriptArgs[i]);
    }
    }

    free(ctx->scriptArgs);

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

    // Reset all values in the stack (see issue #137)
    // Keep in mind that recent GameMaker versions do seem to emit Pop/Popz when exiting loops (example: when using a repeat + return) but older versions DO need it
    unwindVMStack(ctx, storedStackTop);

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
        case INSTANCE_BUILTIN:   return "builtin";
        case INSTANCE_STATIC:    return "static";
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
static void disasmFormatVarComment(const uint8_t* extraData, bool isPop, char* buf, size_t bufSize) {
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
                    snprintf(operandStr, operandSize, "%lld", (longlong) BinaryUtils_readInt64(extraData));
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
                    disasmFormatVarComment(extraData, false, commentStr, commentSize);
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
            disasmFormatVarComment(extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHGLB:
            snprintf(opcodeStr, opcodeSize, "PushGlb.v");
            disasmFormatVar(ctx, extraData, "global", (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHBLTN:
            snprintf(opcodeStr, opcodeSize, "PushBltn.v");
            disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(extraData, false, commentStr, commentSize);
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
            disasmFormatVarComment(extraData, true, commentStr, commentSize);
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
            snprintf(operandStr, operandSize, "%s(argCount=%d)", funcName, argCount);
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
            snprintf(operandStr, operandSize, "argCount=%d", argCount);
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
                case BREAK_ISNULLISH:   mnemonic = "isnullish"; break;
                case BREAK_PUSHREF:     mnemonic = "pushref"; break;
                default:                mnemonic = nullptr; break;
            }
            if (mnemonic != nullptr) {
                snprintf(opcodeStr, opcodeSize, "%s.%c", mnemonic, gmlTypeChar(type1));
                if (breakType == BREAK_PUSHREF) {
                    uint32_t operand = BinaryUtils_readUint32Aligned(extraData);
                    uint8_t assetType = (uint8_t) ((operand >> 24) & 0xFF);
                    int32_t index = (int32_t) (operand & 0x00FFFFFF);
                    if (assetType == ASSET_TYPE_SCRIPT) {
                        const char* funcName = (dw->func.functionCount > (uint32_t) index) ? dw->func.functions[index].name : "???";
                        snprintf(operandStr, operandSize, "script %s", funcName);
                    } else {
                        snprintf(operandStr, operandSize, "asset type=%d index=%d", assetType, index);
                    }
                }
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
                    ptrdiff_t codeMapIdx = shgeti(ctx->codeIndexByName, (char*) funcName);
                    if (codeMapIdx >= 0) {
                        int32_t targetIdx = ctx->codeIndexByName[codeMapIdx].value;
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

struct VMDisasOpcodeEntry { uint32_t key; bool value; };

void VM_disassemble(VMContext* ctx, int32_t codeIndex) {
    DataWin* dw = ctx->dataWin;
    require(dw->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &dw->code.entries[codeIndex];

    // Header
    logInfo("=== %s (length=%u, locals=%u, args=%u) ===\n", code->name, code->length, code->localsCount, code->argumentsCount);

    // CodeLocals
    CodeLocals* locals = resolveCodeLocals(ctx, code->name);
    if (locals != nullptr && locals->localVarCount > 0) {
        logInfo("Locals:");
        repeat(locals->localVarCount, i) {
            if (i > 0) logInfo(",");
            logInfo(" [%u] %s", locals->locals[i].varID, locals->locals[i].name);
        }
        logInfo("\n");
    }

    // Cross-references
    if (ctx->crossRefMap != nullptr) {
        ptrdiff_t mapIdx = hmgeti(ctx->crossRefMap, codeIndex);
        if (mapIdx >= 0) {
            int32_t* callers = ctx->crossRefMap[mapIdx].value;
            logInfo("Called by:");
            for (ptrdiff_t i = 0; arrlen(callers) > i; i++) {
                if (i > 0) logInfo(",");
                logInfo(" %s", dw->code.entries[callers[i]].name);
            }
            logInfo("\n");
        }
    }

    logInfo("\n");

    const uint8_t* bytecodeBase = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
    uint32_t codeLength = code->length;

    // Pass 1: collect branch targets for labels
    struct VMDisasOpcodeEntry *branchTargets = nullptr;
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
            logInfo("  %04X: L_%04X:\n", instrAddr, instrAddr);
        }

        int32_t indent = 2 + envDepth * 4;
        char opcodeStr[32];
        char operandStr[256] = "";
        char commentStr[256] = "";

        formatInstruction(ctx, bytecodeBase, instrAddr, instr, extraData, opcodeStr, sizeof(opcodeStr), operandStr, sizeof(operandStr), commentStr, sizeof(commentStr));

        // Print the formatted line
        if (commentStr[0] != '\0') {
            logInfo("%*s%04X (%6d): [0x%08X] %-16s %-45s %s\n", indent, "", instrAddr, instrAddr, instr, opcodeStr, operandStr, commentStr);
        } else {
            logInfo("%*s%04X (%6d): [0x%08X] %-16s %s\n", indent, "", instrAddr, instrAddr, instr, opcodeStr, operandStr);
        }

        // PushEnv increases depth after printing
        if (opcode == OP_PUSHENV) envDepth++;
    }

    hmfree(branchTargets);
    logInfo("\n");
}

#undef VM_registerBuiltin
void VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func) {
    requireMessageFormatted(__FILE__, __LINE__, shgeti(ctx->builtinMap, name) == -1, "Trying to register an already registered builtin function! Function Name: %s", name);
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

    // Free hash maps
    shfree(ctx->codeIndexByName);
    repeat(shlen(ctx->varNameMap), i) {
        free(ctx->varNameMap[i].key);
    }
    shfree(ctx->varNameMap);
    {
    repeat(shlen(ctx->codeLocalsMap), i) {
        free(ctx->codeLocalsMap[i].key);
    }
    }
    shfree(ctx->codeLocalsMap);

    // Free dedup key strings before freeing the hashmaps
    {
    repeat(shlen(ctx->loggedUnknownFuncs), i) {
        free(ctx->loggedUnknownFuncs[i].key);
    }
    }
    shfree(ctx->loggedUnknownFuncs);
    {
    repeat(shlen(ctx->loggedStubbedFuncs), i) {
        free(ctx->loggedStubbedFuncs[i].key);
    }
    }
    shfree(ctx->loggedStubbedFuncs);
#ifdef ENABLE_VM_TRACING
    shfree(ctx->varReadsToBeTraced);
    shfree(ctx->varWritesToBeTraced);
    shfree(ctx->functionCallsToBeTraced);
    shfree(ctx->alarmsToBeTraced);
    shfree(ctx->instanceLifecyclesToBeTraced);
    shfree(ctx->eventsToBeTraced);
    shfree(ctx->collisionsToBeTraced);
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
    free(ctx->staticStructs);

    // Free per-code varID -> slot maps (BC17+ only; nullptr otherwise).
    if (ctx->codeLocalsSlotMaps != nullptr) {
        repeat(ctx->dataWin->code.count, i) {
            IntIntHashMap_free(&ctx->codeLocalsSlotMaps[i]);
        }
        free(ctx->codeLocalsSlotMaps);
        ctx->codeLocalsSlotMaps = nullptr;
    }

    // Free the global scope instance
    Instance_free(ctx->globalScopeInstance);

#ifndef PLATFORM_PS2
    free(ctx);
#endif
}
