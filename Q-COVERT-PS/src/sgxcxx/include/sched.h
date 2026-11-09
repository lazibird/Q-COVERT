#pragma once

#ifdef __cplusplus
extern "C" {
#endif

static inline int sched_yield(void) { return 0; }

#ifdef __cplusplus
}
#endif
