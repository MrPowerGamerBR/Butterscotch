#pragma once

#include "vm.h"

// ===[ VM functions exposed for JIT ]===
void handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePushLoc(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePushGlb(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePushI(VMContext* ctx, uint32_t instr);
void handlePop(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePopz(VMContext* ctx);
void handleAdd(VMContext* ctx);
void handleSub(VMContext* ctx);
void handleMul(VMContext* ctx);
void handleDiv(VMContext* ctx);
void handleRem(VMContext* ctx);
void handleMod(VMContext* ctx);
void handleAnd(VMContext* ctx);
void handleOr(VMContext* ctx);
void handleXor(VMContext* ctx);
void handleShl(VMContext* ctx);
void handleShr(VMContext* ctx);
void handleNeg(VMContext* ctx);
void handleNot(VMContext* ctx, uint32_t instr);
void handleConv(VMContext* ctx, uint32_t instr);
void handleCmp(VMContext* ctx, uint32_t instr);
void handleDup(VMContext* ctx, uint32_t instr);
void handleBranch(VMContext* ctx, uint32_t instr, uint32_t instrAddr);
void handleBranchTrue(VMContext* ctx, uint32_t instr, uint32_t instrAddr);
void handleBranchFalse(VMContext* ctx, uint32_t instr, uint32_t instrAddr);
void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void handlePushEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr);
void handlePopEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr);

// ===[ Decoding helpers ]===
uint8_t instrOpcode(uint32_t instr);
uint8_t instrType1(uint32_t instr);
uint32_t extraDataSize(uint8_t type1);
bool instrHasExtraData(uint32_t instr);
int32_t instrJumpOffset(uint32_t instr);

// ===[ Stack helpers ]===
void stackPush(VMContext* ctx, RValue val);
RValue stackPop(VMContext* ctx);
RValue* stackPeek(VMContext* ctx);
