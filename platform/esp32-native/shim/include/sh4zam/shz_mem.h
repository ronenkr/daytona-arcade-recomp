#ifndef ESP_SHIM_SHZ_MEM_H
#define ESP_SHIM_SHZ_MEM_H
#include <string.h>
static inline void *shz_memcpy4(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static inline void *shz_memcpy32(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static inline void *shz_memset8(void *d, int v, size_t n) { return memset(d, v, n); }
#endif
