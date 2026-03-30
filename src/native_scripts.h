#pragma once

#include "vm.h"
#include "runner.h"

// Native code function: replaces bytecode execution for a specific code entry.
// Called with the same context as VM_executeCode would be (instance already set on ctx).
typedef void (*NativeCodeFunc)(VMContext* ctx, Runner* runner, Instance* instance);

// Initializes the native script override system. Must be called after VM_create and Runner_create.
// Scans data.win for needed resource indices, variable IDs, and script code IDs.
void NativeScripts_init(VMContext* ctx, Runner* runner);

// Looks up a native override for the given code entry name.
// Returns the native function if one exists, or nullptr if the code should run as bytecode.
NativeCodeFunc NativeScripts_find(const char* codeName);
