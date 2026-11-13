/* A plain-C stand-in for the few SH4ZAM (MIT) calls that gameplay-side code makes, for builds
   without an SH-4. The render side of the Dreamcast port does not build here. */
#ifndef ESP_SHIM_SHZ_H
#define ESP_SHIM_SHZ_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "shz_mem.h"
static inline float shz_fabsf(float x) { return fabsf(x); }
static inline float shz_sqrtf(float x) { return sqrtf(x); }
static inline float shz_divf(float a, float b) { return a / b; }
static inline float shz_lerpf(float a, float b, float t) { return a + (b - a) * t; }
typedef struct { float x, y; } shz_vec2_t;
typedef struct { float x, y, z; } shz_vec3_t;
typedef struct { float x, y, z, w; } shz_vec4_t;
typedef union { float elem[12]; struct { float left[3], up[3], forward[3], pos[3]; }; } shz_mat3x4_t;
static inline shz_vec2_t shz_vec2_init(float x, float y) { return (shz_vec2_t){ x, y }; }
static inline shz_vec3_t shz_vec3_init(float x, float y, float z) { return (shz_vec3_t){ x, y, z }; }
/* The matrix unit's XMTRX as a global 4x4 (column-major), for the few places that load a 3x4 and transform. */
static float shz_xmtrx_m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static inline void shz_xmtrx_load_3x4(const shz_mat3x4_t *m) {
    for (int c = 0; c < 4; ++c) { for (int r = 0; r < 3; ++r) shz_xmtrx_m[c * 4 + r] = m->elem[c * 3 + r]; shz_xmtrx_m[c * 4 + 3] = c == 3; }
}
static inline shz_vec4_t shz_vec4_init(float x, float y, float z, float w) { return (shz_vec4_t){ x, y, z, w }; }
static inline shz_vec4_t shz_xmtrx_transform_vec4(shz_vec4_t v) {
    const float *m = shz_xmtrx_m;
    return (shz_vec4_t){ m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12]*v.w, m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13]*v.w,
                         m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14]*v.w, v.w };
}
static inline float shz_invf_fsrra(float x) { return 1.0f / sqrtf(x); }
/* the 2D screens (render/ui/) */
static inline float shz_divf_fsrra(float a, float b) { return a / b; }
static inline shz_vec2_t shz_vec2_mul(shz_vec2_t a, shz_vec2_t b) { return (shz_vec2_t){ a.x * b.x, a.y * b.y }; }
static inline float shz_vec2_dot(shz_vec2_t a, shz_vec2_t b) { return a.x * b.x + a.y * b.y; }
typedef struct { float sin, cos; } shz_sincos_t;
static inline shz_sincos_t shz_sincosu16(uint16_t angle) {   /* 65536 units a turn, as the SH-4's fsca */
    const float a = (float)angle * (6.2831853071795864f / 65536.0f);
    return (shz_sincos_t){ sinf(a), cosf(a) };
}
static inline void shz_xmtrx_init_scale(float x, float y, float z) {
    memset(shz_xmtrx_m, 0, sizeof shz_xmtrx_m);
    shz_xmtrx_m[0] = x; shz_xmtrx_m[5] = y; shz_xmtrx_m[10] = z; shz_xmtrx_m[15] = 1.0f;
}
static inline shz_vec3_t shz_xmtrx_transform_point3(shz_vec3_t v) {
    const float *m = shz_xmtrx_m;
    return (shz_vec3_t){ m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12], m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13],
                         m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14] };
}
static inline shz_vec3_t shz_xmtrx_transform_vec3(shz_vec3_t v) {
    const float *m = shz_xmtrx_m;
    return (shz_vec3_t){ m[0]*v.x + m[4]*v.y + m[8]*v.z, m[1]*v.x + m[5]*v.y + m[9]*v.z, m[2]*v.x + m[6]*v.y + m[10]*v.z };
}
#endif
