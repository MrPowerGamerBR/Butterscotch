#pragma once

#include "vm.h"

#ifdef USE_JIT
typedef void (*JitFunc)(VMContext* ctx, RValue* result);

void VM_jitCompile(VMContext* ctx, int32_t codeIndex);
void VM_jitFree(CodeEntry* entry);
#endif
