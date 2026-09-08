#ifndef _BS_NOOP_RENDERER_H_
#define _BS_NOOP_RENDERER_H_

#include "common.h"
#include "renderer.h"

// A no-op renderer that silently ignores all rendering calls.
// Useful for GML interpreter benchmarking where rendering can create unnecessary noise
Renderer* NoopRenderer_create(void);

#endif /* _BS_NOOP_RENDERER_H_ */
