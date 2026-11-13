#pragma once
#include <stddef.h>
static inline void *mm_sbrk(long n) { (void)n; return 0; }
