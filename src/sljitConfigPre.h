#ifndef SLJIT_CONFIG_PRE_H_
#define SLJIT_CONFIG_PRE_H_

#ifdef PS2
#include <kernel.h>
#include <malloc.h>

/* PS2-specific SLJIT configuration */
#define SLJIT_UTIL_STACK 0
#define SLJIT_EXECUTABLE_ALLOCATOR 0
#define SLJIT_MALLOC_EXEC(size, exec_allocator_data) malloc(size)
#define SLJIT_FREE_EXEC(ptr, exec_allocator_data) free(ptr)

/* Cache flush for Emotion Engine (MIPS) */
#define SLJIT_CACHE_FLUSH(from, to) FlushCache(0)

#endif

#endif /* SLJIT_CONFIG_PRE_H_ */
