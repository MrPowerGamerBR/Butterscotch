#include "vm_jit.h"
#include "vm_internal.h"
#include "binary_utils.h"
#include "utils.h"
#include "stb_ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#ifdef USE_JIT
#include "sljitLir.h"

// ===[ Forward declarations ]===
void VM_popAndStoreResult(VMContext* ctx, RValue* dest);
void VM_storeUndefined(RValue* dest);
void VM_jitAbort(uint8_t opcode);

typedef struct {
    struct sljit_jump* jump;
    uint32_t target_ip;
} JumpFixup;

void VM_jitCompile(VMContext* ctx, int32_t codeIndex) {
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];
    if (code->jitCode != NULL) return;

    struct sljit_compiler* compiler = sljit_create_compiler(NULL);
    if (!compiler) return;

    // Signature: void jit_func(VMContext* ctx, RValue* out_result)
    // S0 = ctx, S1 = out_result
    sljit_emit_enter(compiler, 0, SLJIT_ARGS2V(W, W), 3, 2, 0);

    uint8_t* bytecode = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);

    // Labels for each instruction offset
    struct sljit_label** labels = safeCalloc(code->length + 1, sizeof(struct sljit_label*));
    JumpFixup* fixups = NULL;

    uint32_t ip = 0;
    while (ip < code->length) {
        labels[ip] = sljit_emit_label(compiler);

        uint32_t instrAddr = ip;
        uint32_t instr = BinaryUtils_readUint32(bytecode + ip);
        ip += 4;

        uint8_t* extraData = bytecode + ip;
        uint32_t eDataSize = 0;
        if (instrHasExtraData(instr)) {
            eDataSize = extraDataSize(instrType1(instr));
            ip += eDataSize;
        }

        // Sync ctx->ip to the address of the NEXT instruction
        sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip), SLJIT_IMM, (sljit_sw)ip);

        uint8_t opcode = instrOpcode(instr);

        switch (opcode) {
            case OP_PUSH:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePush));
                break;
            case OP_PUSHLOC:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePushLoc));
                break;
            case OP_PUSHGLB:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePushGlb));
                break;
            case OP_PUSHBLTN:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePushBltn));
                break;
            case OP_PUSHI:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePushI));
                break;
            case OP_POP:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePop));
                break;
            case OP_POPZ:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePopz));
                break;
            case OP_ADD:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleAdd));
                break;
            case OP_SUB:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleSub));
                break;
            case OP_MUL:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleMul));
                break;
            case OP_DIV:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleDiv));
                break;
            case OP_REM:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleRem));
                break;
            case OP_MOD:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleMod));
                break;
            case OP_AND:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleAnd));
                break;
            case OP_OR:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleOr));
                break;
            case OP_XOR:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleXor));
                break;
            case OP_SHL:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleShl));
                break;
            case OP_SHR:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleShr));
                break;
            case OP_NEG:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleNeg));
                break;
            case OP_NOT:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleNot));
                break;
            case OP_CONV:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleConv));
                break;
            case OP_CMP:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleCmp));
                break;
            case OP_DUP:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleDup));
                break;
            case OP_B: {
                int32_t offset = instrJumpOffset(instr);
                uint32_t target_ip = (uint32_t)((int32_t)instrAddr + offset);
                sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip), SLJIT_IMM, (sljit_sw)target_ip);
                struct sljit_jump* jump = sljit_emit_jump(compiler, SLJIT_JUMP);
                JumpFixup jf = {jump, target_ip};
                arrput(fixups, jf);
                break;
            }
            case OP_BT: {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)instrAddr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleBranchTrue));

                sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip));
                int32_t offset = instrJumpOffset(instr);
                uint32_t target_ip = (uint32_t)((int32_t)instrAddr + offset);

                sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)target_ip);
                struct sljit_jump* jump_to_target = sljit_emit_jump(compiler, SLJIT_EQUAL);
                JumpFixup jf_target = {jump_to_target, target_ip};
                arrput(fixups, jf_target);

                struct sljit_jump* jump_to_next = sljit_emit_jump(compiler, SLJIT_JUMP);
                JumpFixup jf_next = {jump_to_next, ip};
                arrput(fixups, jf_next);
                break;
            }
            case OP_BF: {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)instrAddr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleBranchFalse));

                sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip));
                int32_t offset = instrJumpOffset(instr);
                uint32_t target_ip = (uint32_t)((int32_t)instrAddr + offset);

                sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)target_ip);
                struct sljit_jump* jump_to_target = sljit_emit_jump(compiler, SLJIT_EQUAL);
                JumpFixup jf_target = {jump_to_target, target_ip};
                arrput(fixups, jf_target);

                struct sljit_jump* jump_to_next = sljit_emit_jump(compiler, SLJIT_JUMP);
                JumpFixup jf_next = {jump_to_next, ip};
                arrput(fixups, jf_next);
                break;
            }
            case OP_CALL:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)extraData);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handleCall));
                break;
            case OP_RET:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(VM_popAndStoreResult));
                sljit_emit_return_void(compiler);
                break;
            case OP_EXIT:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(VM_storeUndefined));
                sljit_emit_return_void(compiler);
                break;
            case OP_PUSHENV: {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)instrAddr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePushEnv));

                // PushEnv can jump to the end of the with-block
                sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip));
                int32_t offset = instrJumpOffset(instr);
                uint32_t target_ip = (uint32_t)((int32_t)instrAddr + offset);

                sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)target_ip);
                struct sljit_jump* jump_to_target = sljit_emit_jump(compiler, SLJIT_EQUAL);
                JumpFixup jf_target = {jump_to_target, target_ip};
                arrput(fixups, jf_target);

                struct sljit_jump* jump_to_next = sljit_emit_jump(compiler, SLJIT_JUMP);
                JumpFixup jf_next = {jump_to_next, ip};
                arrput(fixups, jf_next);
                break;
            }
            case OP_POPENV: {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)instr);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)instrAddr);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(W, W, W), SLJIT_IMM, SLJIT_FUNC_ADDR(handlePopEnv));

                // PopEnv can jump back to the start of the with-block
                sljit_emit_op1(compiler, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), offsetof(VMContext, ip));
                int32_t offset = instrJumpOffset(instr);
                uint32_t target_ip = (uint32_t)((int32_t)instrAddr + offset);

                sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)target_ip);
                struct sljit_jump* jump_to_target = sljit_emit_jump(compiler, SLJIT_EQUAL);
                JumpFixup jf_target = {jump_to_target, target_ip};
                arrput(fixups, jf_target);

                struct sljit_jump* jump_to_next = sljit_emit_jump(compiler, SLJIT_JUMP);
                JumpFixup jf_next = {jump_to_next, ip};
                arrput(fixups, jf_next);
                break;
            }
            case OP_BREAK:
                break;
            default:
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)opcode);
                sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(VM_jitAbort));
                break;
        }
    }

    // End of code: return undefined if we somehow fall through
    labels[code->length] = sljit_emit_label(compiler);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
    sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1V(W), SLJIT_IMM, SLJIT_FUNC_ADDR(VM_storeUndefined));
    sljit_emit_return_void(compiler);

    // Resolve jumps
    for (int i = 0; i < arrlen(fixups); i++) {
        if (fixups[i].target_ip < code->length + 1 && labels[fixups[i].target_ip]) {
            sljit_set_label(fixups[i].jump, labels[fixups[i].target_ip]);
        }
    }

    code->jitCode = sljit_generate_code(compiler, 0, NULL);
    sljit_free_compiler(compiler);
    free(labels);
    arrfree(fixups);
}

void VM_jitFree(CodeEntry* entry) {
    if (entry->jitCode) {
        sljit_free_code(entry->jitCode, NULL);
        entry->jitCode = NULL;
    }
}

// ===[ Helpers ]===
void VM_popAndStoreResult(VMContext* ctx, RValue* dest) {
    *dest = stackPop(ctx);
}

void VM_storeUndefined(RValue* dest) {
    *dest = RValue_makeUndefined();
}

void VM_jitAbort(uint8_t opcode) {
    fprintf(stderr, "VM JIT: Unsupported opcode 0x%02x\n", opcode);
    abort();
}

#endif
