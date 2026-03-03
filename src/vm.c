#include "vm.h"
#include "vm_builtins.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"

// ===[ Stack Operations ]===

static void stackPush(VMStack* stack, RValue val) {
    require(VM_STACK_SIZE > stack->top);
    stack->slots[stack->top++] = val;
}

static RValue stackPop(VMStack* stack) {
    require(stack->top > 0);
    return stack->slots[--stack->top];
}

static RValue* stackPeek(VMStack* stack) {
    require(stack->top > 0);
    return &stack->slots[stack->top - 1];
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

// Read a little-endian uint32 from a byte pointer
static uint32_t readUint32(const uint8_t* p) {
    uint32_t val;
    memcpy(&val, p, 4);
    return val;
}


// Read a little-endian int32 from a byte pointer
static int32_t readInt32(const uint8_t* p) {
    int32_t val;
    memcpy(&val, p, 4);
    return val;
}

// Read a little-endian double from a byte pointer
static double readFloat64(const uint8_t* p) {
    double val;
    memcpy(&val, p, 8);
    return val;
}

// Read a little-endian float from a byte pointer
static float readFloat32(const uint8_t* p) {
    float val;
    memcpy(&val, p, 4);
    return val;
}

// Read a little-endian int64 from a byte pointer
static int64_t readInt64(const uint8_t* p) {
    int64_t val;
    memcpy(&val, p, 8);
    return val;
}

// ===[ Reference Chain Resolution ]===

// Walks reference chains from the unmodified file buffer and builds hash maps
// mapping absolute file offsets to resolved operand values.
// The file buffer stays completely read-only.
static void buildReferenceMaps(VMContext* ctx) {
    DataWin* dataWin = ctx->dataWin;
    uint8_t* buf = dataWin->fileBuffer;

    ctx->varRefMap = nullptr;
    ctx->funcRefMap = nullptr;

    // Build variable reference map
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* v = &dataWin->vari.variables[varIdx];
        if (v->occurrences == 0) continue;

        uint32_t addr = v->firstAddress;
        repeat(v->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = readUint32(&buf[operandAddr]);
            uint32_t delta = operand & 0x07FFFFFF;
            uint32_t upperBits = operand & 0xF8000000;

            // Store resolved operand: upper bits preserved, lower 27 = varIdx
            hmput(ctx->varRefMap, operandAddr, upperBits | (varIdx & 0x07FFFFFF));

            if (v->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }

    // Build function reference map
    repeat(dataWin->func.functionCount, funcIdx) {
        Function* f = &dataWin->func.functions[funcIdx];
        if (f->occurrences == 0) continue;

        uint32_t addr = f->firstAddress;
        repeat(f->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = readUint32(&buf[operandAddr]);
            uint32_t delta = operand & 0x07FFFFFF;

            hmput(ctx->funcRefMap, operandAddr, funcIdx);

            if (f->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }
}

// Resolve a variable operand: returns upper bits | varIndex (same format as old patched operand)
static uint32_t resolveVarOperand(VMContext* ctx, const uint8_t* extraData) {
    uint32_t absoluteOffset = (uint32_t)(extraData - ctx->dataWin->fileBuffer);
    return hmget(ctx->varRefMap, absoluteOffset);
}

// Resolve a function operand: returns funcIndex
static uint32_t resolveFuncOperand(VMContext* ctx, const uint8_t* extraData) {
    uint32_t absoluteOffset = (uint32_t)(extraData - ctx->dataWin->fileBuffer);
    return hmget(ctx->funcRefMap, absoluteOffset);
}

// ===[ Variable Resolution ]===

static RValue resolveVariableRead(VMContext* ctx, int16_t instanceType, uint32_t varRef) {
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];

    // Check for array access
    uint8_t varType = (varRef >> 24) & 0xFF;
    if (varType == VARTYPE_ARRAY || varType == VARTYPE_STACKTOP) {
        fprintf(stderr, "VM: Array/StackTop variable access not yet implemented (variable '%s', varType=0x%02X)\n", varDef->name, varType);
        return RValue_makeUndefined();
    }

    switch (instanceType) {
        case INSTANCE_LOCAL:
            require(ctx->localVarCount > (uint32_t) varDef->varID);
            return ctx->localVars[varDef->varID];
        case INSTANCE_GLOBAL:
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            return ctx->globalVars[varDef->varID];
        case INSTANCE_SELF:
            require(ctx->selfVarCount > (uint32_t) varDef->varID);
            return ctx->selfVars[varDef->varID];
        default:
            fprintf(stderr, "VM: Unhandled instance type %d for variable read '%s'\n", instanceType, varDef->name);
            return RValue_makeUndefined();
    }
}

static RValue* resolveVariableWrite(VMContext* ctx, int16_t instanceType, uint32_t varRef) {
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];

    // Check for array access
    uint8_t varType = (varRef >> 24) & 0xFF;
    if (varType == VARTYPE_ARRAY || varType == VARTYPE_STACKTOP) {
        fprintf(stderr, "VM: Array/StackTop variable write not yet implemented (variable '%s', varType=0x%02X)\n", varDef->name, varType);
        abort();
    }

    switch (instanceType) {
        case INSTANCE_LOCAL:
            require(ctx->localVarCount > (uint32_t) varDef->varID);
            return &ctx->localVars[varDef->varID];
        case INSTANCE_GLOBAL:
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            return &ctx->globalVars[varDef->varID];
        case INSTANCE_SELF:
            require(ctx->selfVarCount > (uint32_t) varDef->varID);
            return &ctx->selfVars[varDef->varID];
        default:
            fprintf(stderr, "VM: Unhandled instance type %d for variable write '%s'\n", instanceType, varDef->name);
            abort();
    }
}

// ===[ Type Conversion ]===

static RValue convertValue(RValue val, uint8_t targetType) {
    switch (targetType) {
        case GML_TYPE_DOUBLE:
            return RValue_makeReal(RValue_toReal(val));
        case GML_TYPE_FLOAT:
            return RValue_makeReal((double) (float) RValue_toReal(val));
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

// ===[ Forward Declarations ]===

static RValue executeLoop(VMContext* ctx);
static void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData);

// ===[ Opcode Handlers ]===

static void handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    uint8_t type1 = instrType1(instr);

    switch (type1) {
        case GML_TYPE_DOUBLE:
            stackPush(&ctx->stack, RValue_makeReal(readFloat64(extraData)));
            break;
        case GML_TYPE_FLOAT:
            stackPush(&ctx->stack, RValue_makeReal((double) readFloat32(extraData)));
            break;
        case GML_TYPE_INT32:
            stackPush(&ctx->stack, RValue_makeInt32(readInt32(extraData)));
            break;
        case GML_TYPE_INT64:
            stackPush(&ctx->stack, RValue_makeInt64(readInt64(extraData)));
            break;
        case GML_TYPE_BOOL:
            stackPush(&ctx->stack, RValue_makeBool(readInt32(extraData) != 0));
            break;
        case GML_TYPE_VARIABLE: {
            int16_t instanceType = instrInstanceType(instr);
            uint32_t varRef = resolveVarOperand(ctx, extraData);
            RValue val = resolveVariableRead(ctx, instanceType, varRef);
            stackPush(&ctx->stack, val);
            break;
        }
        case GML_TYPE_STRING: {
            int32_t stringIndex = readInt32(extraData);
            require(stringIndex >= 0 && ctx->dataWin->strg.count > (uint32_t) stringIndex);
            stackPush(&ctx->stack, RValue_makeString(ctx->dataWin->strg.strings[stringIndex]));
            break;
        }
        case GML_TYPE_INT16: {
            int16_t value = (int16_t) (instr & 0xFFFF);
            stackPush(&ctx->stack, RValue_makeInt32((int32_t) value));
            break;
        }
        default:
            fprintf(stderr, "VM: Push with unknown type 0x%X\n", type1);
            abort();
    }
}

static void handlePushLoc(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    (void) instr;
    uint32_t varRef = resolveVarOperand(ctx, extraData);
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    require(ctx->localVarCount > (uint32_t) varDef->varID);
    stackPush(&ctx->stack, ctx->localVars[varDef->varID]);
}

static void handlePushGlb(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    (void) instr;
    uint32_t varRef = resolveVarOperand(ctx, extraData);
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    require(ctx->globalVarCount > (uint32_t) varDef->varID);
    stackPush(&ctx->stack, ctx->globalVars[varDef->varID]);
}

static void handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    (void) instr;
    uint32_t varRef = resolveVarOperand(ctx, extraData);
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    fprintf(stderr, "VM: PushBltn not implemented - built-in variable '%s' (varID=%d)\n", varDef->name, varDef->varID);
    stackPush(&ctx->stack, RValue_makeReal(0.0));
}

static void handlePushI(VMContext* ctx, uint32_t instr) {
    int16_t value = (int16_t) (instr & 0xFFFF);
    stackPush(&ctx->stack, RValue_makeInt32((int32_t) value));
}

static void handlePop(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    int16_t instanceType = instrInstanceType(instr);
    uint8_t type1 = instrType1(instr);   // destination type
    uint8_t type2 = instrType2(instr);   // source type (what's on stack)
    uint32_t varRef = resolveVarOperand(ctx, extraData);

    RValue val = stackPop(&ctx->stack);

    // Convert if source type differs from destination type
    if (type2 != type1 && type1 != GML_TYPE_VARIABLE) {
        RValue converted = convertValue(val, type1);
        RValue_free(&val);
        val = converted;
    }

    RValue* dest = resolveVariableWrite(ctx, instanceType, varRef);

    // Free old value if it owns a string
    RValue_free(dest);

    *dest = val;
}

static void handlePopz(VMContext* ctx) {
    RValue val = stackPop(&ctx->stack);
    RValue_free(&val);
}

static void handleAdd(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);

    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        // String concatenation
        const char* sa = a.string != nullptr ? a.string : "";
        const char* sb = b.string != nullptr ? b.string : "";
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = malloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(&ctx->stack, RValue_makeOwnedString(result));
    } else if (a.type == RVALUE_STRING || b.type == RVALUE_STRING) {
        // String + Number: convert both to strings and concatenate (GMS behavior)
        char* sa = RValue_toString(a);
        char* sb = RValue_toString(b);
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = malloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        free(sa);
        free(sb);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(&ctx->stack, RValue_makeOwnedString(result));
    } else {
        double result = RValue_toReal(a) + RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(&ctx->stack, RValue_makeReal(result));
    }
}

static void handleSub(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    double result = RValue_toReal(a) - RValue_toReal(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeReal(result));
}

static void handleMul(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);

    if (a.type == RVALUE_STRING) {
        // String * Number = string repetition
        int count = RValue_toInt32(b);
        const char* str = a.string != nullptr ? a.string : "";
        size_t len = strlen(str);
        if (count <= 0 || len == 0) {
            RValue_free(&a);
            RValue_free(&b);
            stackPush(&ctx->stack, RValue_makeOwnedString(strdup("")));
        } else {
            char* result = malloc(len * count + 1);
            repeat(count, i) {
                memcpy(result + i * len, str, len);
            }
            result[len * count] = '\0';
            RValue_free(&a);
            RValue_free(&b);
            stackPush(&ctx->stack, RValue_makeOwnedString(result));
        }
    } else {
        double result = RValue_toReal(a) * RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(&ctx->stack, RValue_makeReal(result));
    }
}

static void handleDiv(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    double divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoDiv :: Divide by zero\n");
        abort();
    }
    double result = RValue_toReal(a) / divisor;
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeReal(result));
}

static void handleRem(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t ib = RValue_toInt32(b);
    if (ib == 0) {
        fprintf(stderr, "VM: DoRem :: Divide by zero\n");
        abort();
    }
    int32_t result = RValue_toInt32(a) % ib;
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleMod(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    double divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoMod :: Divide by zero\n");
        abort();
    }
    double result = fmod(RValue_toReal(a), divisor);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeReal(result));
}

static void handleAnd(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t result = RValue_toInt32(a) & RValue_toInt32(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleOr(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t result = RValue_toInt32(a) | RValue_toInt32(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleXor(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t result = RValue_toInt32(a) ^ RValue_toInt32(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleNeg(VMContext* ctx) {
    RValue a = stackPop(&ctx->stack);
    double result = -RValue_toReal(a);
    RValue_free(&a);
    stackPush(&ctx->stack, RValue_makeReal(result));
}

static void handleNot(VMContext* ctx) {
    RValue a = stackPop(&ctx->stack);
    int32_t result = ~RValue_toInt32(a);
    RValue_free(&a);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleShl(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t result = RValue_toInt32(a) << RValue_toInt32(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleShr(VMContext* ctx) {
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);
    int32_t result = RValue_toInt32(a) >> RValue_toInt32(b);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeInt32(result));
}

static void handleConv(VMContext* ctx, uint32_t instr) {
    uint8_t srcType = instrType1(instr);
    uint8_t dstType = instrType2(instr);

    RValue val = stackPop(&ctx->stack);

    uint8_t convKey = (dstType << 4) | srcType;
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
        case 0x60: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF0: result = RValue_makeInt32((int32_t) val.real); break;

        // Float (1) -> other (float stored as double in our RValue)
        case 0x01: result = RValue_makeReal(val.real); break;
        case 0x21: result = RValue_makeInt32((int32_t) val.real); break;
        case 0x31: result = RValue_makeInt64((int64_t) val.real); break;
        case 0x41: result = RValue_makeBool(val.real > 0.5); break;

        // Int32 (2) -> other
        case 0x02: result = RValue_makeReal((double) val.int32); break;
        case 0x12: result = RValue_makeReal((double) val.int32); break;
        case 0x32: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x42: result = RValue_makeBool(val.int32 > 0); break;
        case 0x62: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF2: result = val; break;

        // Int64 (3) -> other
        case 0x03: result = RValue_makeReal((double) val.int64); break;
        case 0x23: result = RValue_makeInt32((int32_t) val.int64); break;
        case 0x43: result = RValue_makeBool(val.int64 > 0); break;

        // Bool (4) -> other
        case 0x04: result = RValue_makeReal((double) val.int32); break;
        case 0x24: result = RValue_makeInt32(val.int32); break;
        case 0x34: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x64: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }

        // Variable (5) -> other
        case 0x05: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x15: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x25: result = RValue_makeInt32(RValue_toInt32(val)); break;
        case 0x35: result = RValue_makeInt64(RValue_toInt64(val)); break;
        case 0x45: result = RValue_makeBool(RValue_toBool(val)); break;
        case 0x65: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF5: result = RValue_makeInt32(RValue_toInt32(val)); break;

        // String (6) -> other
        case 0x06: result = RValue_makeReal(strtod(val.string, nullptr)); break;
        case 0x26: result = RValue_makeInt32((int32_t) strtod(val.string, nullptr)); break;
        case 0x36: result = RValue_makeInt64((int64_t) strtod(val.string, nullptr)); break;
        case 0x46: result = RValue_makeBool(val.string != nullptr && val.string[0] != '\0'); break;
        case 0x56: {
            // String -> Variable: keep as-is since our RValue handles strings natively
            result = val;
            break;
        }

        // Int16 (F) -> other
        case 0x0F: result = RValue_makeReal((double) val.int32); break;
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

    stackPush(&ctx->stack, result);
}

static void handleCmp(VMContext* ctx, uint32_t instr) {
    uint8_t cmpKind = instrCmpKind(instr);
    RValue b = stackPop(&ctx->stack);
    RValue a = stackPop(&ctx->stack);

    bool result;
    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
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
        double da = RValue_toReal(a);
        double db = RValue_toReal(b);
        switch (cmpKind) {
            case CMP_LT:  result = da < db; break;
            case CMP_LTE: result = da <= db; break;
            case CMP_EQ:  result = da == db; break;
            case CMP_NEQ: result = da != db; break;
            case CMP_GTE: result = da >= db; break;
            case CMP_GT:  result = da > db; break;
            default: result = false; break;
        }
    }

    RValue_free(&a);
    RValue_free(&b);
    stackPush(&ctx->stack, RValue_makeBool(result));
}

static void handleDup(VMContext* ctx, uint32_t instr) {
    (void) instr;
    RValue* top = stackPeek(&ctx->stack);
    RValue copy = *top;

    // If the value owns a string, duplicate it to avoid double-free
    if (copy.type == RVALUE_STRING && copy.ownsString && copy.string != nullptr) {
        copy.string = strdup(copy.string);
    }

    stackPush(&ctx->stack, copy);
}

static void handleBranch(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    int32_t offset = instrJumpOffset(instr);
    ctx->ip = instrAddr + offset;
}

static void handleBranchTrue(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    RValue val = stackPop(&ctx->stack);
    bool condition = RValue_toBool(val);
    RValue_free(&val);
    if (condition) {
        int32_t offset = instrJumpOffset(instr);
        ctx->ip = instrAddr + offset;
    }
}

static void handleBranchFalse(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    RValue val = stackPop(&ctx->stack);
    bool condition = RValue_toBool(val);
    RValue_free(&val);
    if (!condition) {
        int32_t offset = instrJumpOffset(instr);
        ctx->ip = instrAddr + offset;
    }
}

// ===[ Function Call Handler ]===

static void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    int32_t argCount = instr & 0xFFFF;
    uint32_t funcIndex = resolveFuncOperand(ctx, extraData);
    require(ctx->dataWin->func.functionCount > funcIndex);

    const char* funcName = ctx->dataWin->func.functions[funcIndex].name;

    // Pop arguments from stack (they were pushed left-to-right, so pop in reverse)
    RValue* args = nullptr;
    if (argCount > 0) {
        args = malloc(argCount * sizeof(RValue));
        for (int32_t i = argCount - 1; i >= 0; i--) {
            args[i] = stackPop(&ctx->stack);
        }
    }

    // Check built-in functions first
    BuiltinFunc builtin = VMBuiltins_find(funcName);
    if (builtin != nullptr) {
        RValue result = builtin(ctx, args, argCount);
        // Free arguments
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            free(args);
        }
        stackPush(&ctx->stack, result);
        return;
    }

    // Look up script/user function via funcMap
    ptrdiff_t mapIdx = shgeti(ctx->funcMap, (char*) funcName);
    if (0 > mapIdx) {
        // Log once per (callingCode, funcName) pair
        const char* callerName = VM_getCallerName(ctx);
        char* dedupKey = VM_createDedupKey(callerName, funcName);

        if (0 > shgeti(ctx->loggedUnknownFuncs, dedupKey)) {
            shput(ctx->loggedUnknownFuncs, dedupKey, true);
            fprintf(stderr, "VM: Unknown function '%s' called from '%s'!\n", funcName, callerName);
        }
        free(dedupKey);

        // Free arguments and push undefined
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            free(args);
        }
        stackPush(&ctx->stack, RValue_makeUndefined());
        return;
    }

    int32_t codeIndex = ctx->funcMap[mapIdx].value;
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    // Save current frame
    CallFrame* frame = malloc(sizeof(CallFrame));
    frame->savedIP = ctx->ip;
    frame->savedCodeEnd = ctx->codeEnd;
    frame->savedBytecodeBase = ctx->bytecodeBase;
    frame->savedLocals = ctx->localVars;
    frame->savedLocalsCount = ctx->localVarCount;
    frame->savedCodeName = ctx->currentCodeName;
    frame->parent = ctx->callStack;
    ctx->callStack = frame;
    ctx->callDepth++;

    // Set up callee
    ctx->bytecodeBase = ctx->dataWin->fileBuffer + code->bytecodeAbsoluteOffset;
    ctx->ip = 0;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;

    uint32_t localsCount = code->localsCount;
    if ((uint32_t) argCount > localsCount) localsCount = (uint32_t) argCount;
    ctx->localVars = calloc(localsCount, sizeof(RValue));
    ctx->localVarCount = localsCount;

    // Initialize all locals to RVALUE_UNDEFINED
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Copy arguments into the first N local slots via their varIDs
    // In GMS, arguments map to the 'argument0'..'argumentN' variables which have specific varIDs.
    // For simplicity, we look up argument variable indices from the code locals.
    // The arguments are stored with names like "arguments" or "argument0"..."argument15".
    // However, in GMS 1.4 bytecode, argument values are typically accessed as local variables.
    // The callee's code will use PushLoc with the argument variable IDs to read them.
    // We need to figure out which varIDs correspond to argument0..argumentN.
    // The CodeLocals for this function lists all locals including arguments.
    // Let's find the code locals entry and map argument names to varIDs.

    // Find the CodeLocals for this code entry
    CodeLocals* codeLocals = nullptr;
    forEach(CodeLocals, cl, ctx->dataWin->func.codeLocals, ctx->dataWin->func.codeLocalsCount) {
        if (strcmp(cl->name, code->name) == 0) {
            codeLocals = cl;
            break;
        }
    }

    if (codeLocals != nullptr && args != nullptr) {
        repeat(argCount, argIdx) {
            // Build expected argument name
            char argName[32];
            snprintf(argName, sizeof(argName), "argument%d", argIdx);

            // Find the local variable with this name
            forEach(LocalVar, local, codeLocals->locals, codeLocals->localVarCount) {
                if (strcmp(local->name, argName) == 0) {
                    uint32_t varID = local->index;
                    if (localsCount > varID) {
                        ctx->localVars[varID] = args[argIdx];
                        // Clear ownsString on the arg so we don't double-free
                        args[argIdx].ownsString = false;
                    }
                    break;
                }
            }
        }
    }

    // Free the args array (values were moved into locals)
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        free(args);
    }

    // Execute the callee
    RValue result = executeLoop(ctx);

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

    ctx->localVars = saved->savedLocals;
    ctx->localVarCount = saved->savedLocalsCount;
    ctx->currentCodeName = saved->savedCodeName;
    ctx->callStack = saved->parent;
    ctx->callDepth--;
    free(saved);

    // Push return value
    stackPush(&ctx->stack, result);
}

// ===[ Execution Loop ]===

static RValue executeLoop(VMContext* ctx) {
    while (ctx->codeEnd > ctx->ip) {
        uint32_t instrAddr = ctx->ip;
        uint32_t instr = readUint32(ctx->bytecodeBase + ctx->ip);
        ctx->ip += 4;

        // extraData pointer (may not be used depending on opcode)
        const uint8_t* extraData = ctx->bytecodeBase + ctx->ip;

        // If instruction has extra data (bit 30 set), advance IP past it
        if (instrHasExtraData(instr)) {
            ctx->ip += extraDataSize(instrType1(instr));
        }

        uint8_t opcode = instrOpcode(instr);

        switch (opcode) {
            // Push instructions
            case OP_PUSH:
                handlePush(ctx, instr, extraData);
                break;
            case OP_PUSHLOC:
                handlePushLoc(ctx, instr, extraData);
                break;
            case OP_PUSHGLB:
                handlePushGlb(ctx, instr, extraData);
                break;
            case OP_PUSHBLTN:
                handlePushBltn(ctx, instr, extraData);
                break;
            case OP_PUSHI:
                handlePushI(ctx, instr);
                break;

            // Pop instructions
            case OP_POP:
                handlePop(ctx, instr, extraData);
                break;
            case OP_POPZ:
                handlePopz(ctx);
                break;

            // Arithmetic
            case OP_ADD: handleAdd(ctx); break;
            case OP_SUB: handleSub(ctx); break;
            case OP_MUL: handleMul(ctx); break;
            case OP_DIV: handleDiv(ctx); break;
            case OP_REM: handleRem(ctx); break;
            case OP_MOD: handleMod(ctx); break;

            // Bitwise / Logical
            case OP_AND: handleAnd(ctx); break;
            case OP_OR:  handleOr(ctx); break;
            case OP_XOR: handleXor(ctx); break;
            case OP_SHL: handleShl(ctx); break;
            case OP_SHR: handleShr(ctx); break;

            // Unary
            case OP_NEG: handleNeg(ctx); break;
            case OP_NOT: handleNot(ctx); break;

            // Type conversion
            case OP_CONV:
                handleConv(ctx, instr);
                break;

            // Comparison
            case OP_CMP:
                handleCmp(ctx, instr);
                break;

            // Duplicate
            case OP_DUP:
                handleDup(ctx, instr);
                break;

            // Branches
            case OP_B:
                handleBranch(ctx, instr, instrAddr);
                break;
            case OP_BT:
                handleBranchTrue(ctx, instr, instrAddr);
                break;
            case OP_BF:
                handleBranchFalse(ctx, instr, instrAddr);
                break;

            // Function call
            case OP_CALL:
                handleCall(ctx, instr, extraData);
                break;

            // Return
            case OP_RET: {
                RValue retVal = stackPop(&ctx->stack);
                return retVal;
            }

            // Exit (no return value)
            case OP_EXIT:
                return RValue_makeUndefined();

            // Environment (with-statements) - stubbed
            case OP_PUSHENV:
                fprintf(stderr, "VM: PushEnv (with-statement) not implemented. Aborting.\n");
                abort();
            case OP_POPENV:
                fprintf(stderr, "VM: PopEnv (with-statement) not implemented. Aborting.\n");
                abort();

            // Break (no-op / debug)
            case OP_BREAK:
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
    VMContext* ctx = calloc(1, sizeof(VMContext));
    ctx->dataWin = dataWin;
    ctx->stack.top = 0;
    ctx->selfId = -1;
    ctx->otherId = -1;
    ctx->callDepth = 0;

    // Build reference lookup maps (file buffer stays read-only)
    buildReferenceMaps(ctx);

    // Scan VARI entries to find max varID for each scope
    // Built-in variables have varID == -6 (sentinel), skip those
    uint32_t maxGlobalVarID = 0;
    uint32_t maxSelfVarID = 0;
    forEach(Variable, v, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (0 > v->varID) continue;
        if (v->instanceType == INSTANCE_GLOBAL) {
            if ((uint32_t) v->varID + 1 > maxGlobalVarID) maxGlobalVarID = (uint32_t) v->varID + 1;
        } else if (v->instanceType == INSTANCE_SELF) {
            if ((uint32_t) v->varID + 1 > maxSelfVarID) maxSelfVarID = (uint32_t) v->varID + 1;
        }
    }

    ctx->globalVarCount = maxGlobalVarID;
    ctx->globalVars = calloc(maxGlobalVarID, sizeof(RValue));
    repeat(maxGlobalVarID, i) {
        ctx->globalVars[i].type = RVALUE_UNDEFINED;
    }

    ctx->selfVarCount = maxSelfVarID;
    ctx->selfVars = calloc(maxSelfVarID, sizeof(RValue));
    repeat(maxSelfVarID, i) {
        ctx->selfVars[i].type = RVALUE_UNDEFINED;
    }

    // Build funcName -> codeIndex hash map from SCPT chunk
    ctx->funcMap = nullptr;
    forEach(Script, s, dataWin->scpt.scripts, dataWin->scpt.count) {
        if (s->name != nullptr && s->codeId >= 0) {
            // Script names map to code entries. The code entry name is typically
            // "gml_Script_<scriptName>", so we store with the code entry's actual name
            // since that's what the FUNC chunk references.
            if (dataWin->code.count > (uint32_t) s->codeId) {
                const char* codeName = dataWin->code.entries[s->codeId].name;
                shput(ctx->funcMap, (char*) codeName, s->codeId);
            }
        }
    }

    // Also map code entry names directly for non-script code (object events, room creation codes, etc.)
    // These may be called by the Call instruction using their FUNC name
    repeat(dataWin->code.count, i) {
        const char* codeName = dataWin->code.entries[i].name;
        ptrdiff_t existing = shgeti(ctx->funcMap, (char*) codeName);
        if (0 > existing) {
            shput(ctx->funcMap, (char*) codeName, (int32_t) i);
        }
    }

    // Register built-in functions
    VMBuiltins_registerAll();

    printf("VM: Initialized with %u global vars, %u self vars, %u functions mapped\n", ctx->globalVarCount, ctx->selfVarCount, (uint32_t) shlen(ctx->funcMap));

    return ctx;
}

RValue VM_executeCode(VMContext* ctx, int32_t codeIndex) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    ctx->bytecodeBase = ctx->dataWin->fileBuffer + code->bytecodeAbsoluteOffset;
    ctx->ip = 0;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;

    // Allocate locals
    uint32_t localsCount = code->localsCount;
    if (localsCount == 0) localsCount = 1; // at least 1 slot to avoid nullptr
    ctx->localVars = calloc(localsCount, sizeof(RValue));
    ctx->localVarCount = localsCount;
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Reset stack for top-level execution
    ctx->stack.top = 0;

    RValue result = executeLoop(ctx);

    // Free locals
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }
    free(ctx->localVars);
    ctx->localVars = nullptr;
    ctx->localVarCount = 0;

    return result;
}

void VM_free(VMContext* ctx) {
    if (ctx == nullptr) return;

    // Free global vars
    if (ctx->globalVars != nullptr) {
        repeat(ctx->globalVarCount, i) {
            RValue_free(&ctx->globalVars[i]);
        }
        free(ctx->globalVars);
    }

    // Free self vars
    if (ctx->selfVars != nullptr) {
        repeat(ctx->selfVarCount, i) {
            RValue_free(&ctx->selfVars[i]);
        }
        free(ctx->selfVars);
    }

    // Free hash maps
    shfree(ctx->funcMap);
    shfree(ctx->loggedUnknownFuncs);
    shfree(ctx->loggedStubbedFuncs);
    hmfree(ctx->varRefMap);
    hmfree(ctx->funcRefMap);

    // Free any remaining call frames
    CallFrame* frame = ctx->callStack;
    while (frame != nullptr) {
        CallFrame* parent = frame->parent;
        free(frame);
        frame = parent;
    }

    free(ctx);
}
