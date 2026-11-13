/* The part of the Dreamcast's PVR interface (KallistiOS dc/pvr.h, KOS License; enDjinn's texture loader, MIT)
   that the native game's 2D screens use, for the console: the headers and vertices they build are the PVR's own
   TA records, bit for bit (the HUD and the attract layer write some words by hand), captured each frame by
   shim/pvr_lite.c and drawn over the picture by shim/pvr_draw.c. */
#ifndef ESP_SHIM_PVR_LITE_H
#define ESP_SHIM_PVR_LITE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void *pvr_ptr_t;
typedef int pvr_list_t;
typedef int pvr_depthcmp_mode_t;
typedef int pvr_filter_mode_t;
enum { PVR_LIST_OP_POLY = 0, PVR_LIST_OP_MOD, PVR_LIST_TR_POLY, PVR_LIST_TR_MOD, PVR_LIST_PT_POLY };

#define PVR_CMD_POLYHDR     0x80840000u
#define PVR_CMD_VERTEX      0xe0000000u
#define PVR_CMD_VERTEX_EOL  0xf0000000u
#define PVR_CMD_USERCLIP    0x20000000u
#define PVR_CMD_MODIFIER    0x80000000u
#define PVR_CMD_SPRITE      0xA0000000u

/* header word fields (KOS dc/pvr.h, PVR_TA_*) as shift and width */
#define PVR_LITE_FIELD(v, shift, bits) (((uint32_t)(v) & ((1u << (bits)) - 1u)) << (shift))
#define PVR_LITE_GET(w, shift, bits) (((w) >> (shift)) & ((1u << (bits)) - 1u))

enum { PVR_DEPTHCMP_NEVER, PVR_DEPTHCMP_LESS, PVR_DEPTHCMP_EQUAL, PVR_DEPTHCMP_LEQUAL, PVR_DEPTHCMP_GREATER,
       PVR_DEPTHCMP_NOTEQUAL, PVR_DEPTHCMP_GEQUAL, PVR_DEPTHCMP_ALWAYS };
enum { PVR_CULLING_NONE, PVR_CULLING_SMALL, PVR_CULLING_CCW, PVR_CULLING_CW };
enum { PVR_BLEND_ZERO, PVR_BLEND_ONE, PVR_BLEND_DESTCOLOR, PVR_BLEND_INVDESTCOLOR, PVR_BLEND_SRCALPHA,
       PVR_BLEND_INVSRCALPHA, PVR_BLEND_DESTALPHA, PVR_BLEND_INVDESTALPHA };
enum { PVR_TXRENV_REPLACE, PVR_TXRENV_MODULATE, PVR_TXRENV_DECAL, PVR_TXRENV_MODULATEALPHA };
#define PVR_FILTER_NONE 0
#define PVR_FILTER_NEAREST 0
#define PVR_FILTER_BILINEAR 2
#define PVR_FILTER_TRILINEAR1 4
#define PVR_FILTER_TRILINEAR2 6
#define PVR_DEPTHWRITE_ENABLE 0
#define PVR_DEPTHWRITE_DISABLE 1
#define PVR_SHADE_FLAT 0
#define PVR_SHADE_GOURAUD 1
#define PVR_CLRFMT_ARGBPACKED 0
#define PVR_CLRFMT_4FLOATS 1
#define PVR_CLRFMT_INTENSITY 2
#define PVR_CLRFMT_INTENSITY_PREV 3
#define PVR_UVFMT_32BIT 0
#define PVR_UVFMT_16BIT 1
#define PVR_UVFLIP_NONE 0
#define PVR_UVCLAMP_NONE 0
#define PVR_UVCLAMP_UV 3
#define PVR_MIPBIAS_NORMAL 4
#define PVR_FOG_DISABLE 2
#define PVR_BLEND_DISABLE 0
#define PVR_BLEND_ENABLE 1
#define PVR_ALPHA_DISABLE 0
#define PVR_ALPHA_ENABLE 1
#define PVR_TXRALPHA_ENABLE 0
#define PVR_TXRALPHA_DISABLE 1

#define PVR_TXRFMT_NONE         0u
#define PVR_TXRFMT_VQ_ENABLE    (1u << 30)
#define PVR_TXRFMT_ARGB1555     (0u << 27)
#define PVR_TXRFMT_RGB565       (1u << 27)
#define PVR_TXRFMT_ARGB4444     (2u << 27)
#define PVR_TXRFMT_YUV422       (3u << 27)
#define PVR_TXRFMT_BUMP         (4u << 27)
#define PVR_TXRFMT_PAL4BPP      (5u << 27)
#define PVR_TXRFMT_PAL8BPP      (6u << 27)
#define PVR_TXRFMT_TWIDDLED     (0u << 26)
#define PVR_TXRFMT_NONTWIDDLED  (1u << 26)
#define PVR_TXRFMT_8BPP_PAL(x)  ((uint32_t)(x) << 25)
#define PVR_TXRFMT_4BPP_PAL(x)  ((uint32_t)(x) << 21)

enum { PVR_PAL_ARGB1555, PVR_PAL_RGB565, PVR_PAL_ARGB4444, PVR_PAL_ARGB8888 };
typedef int pvr_palfmt_t;

/* 16-bit u, v: the top halves of the floats */
static inline uint32_t pvr_lite_float_bits(float f) { union { float f; uint32_t u; } b = { f }; return b.u; }
#define PVR_PACK_16BIT_UV(u, v) ((pvr_lite_float_bits(u) & 0xffff0000u) | (pvr_lite_float_bits(v) >> 16))

typedef struct { uint32_t cmd, mode1, mode2, mode3, d1, d2, d3, d4; } pvr_poly_hdr_t;
typedef struct { uint32_t cmd, mode1, mode2, mode3, argb, oargb, d1, d2; } pvr_sprite_hdr_t;
typedef struct { uint32_t flags; float x, y, z, u, v; uint32_t argb, oargb; } pvr_vertex_t;
typedef struct { uint32_t flags; float ax, ay, az, bx, by, bz, cx, cy, cz, dx, dy; uint32_t dummy, auv, buv, cuv; } pvr_sprite_txr_t;
typedef struct { uint32_t flags; float ax, ay, az, bx, by, bz, cx, cy, cz, dx, dy; uint32_t d1, d2, d3, d4; } pvr_sprite_col_t;

typedef struct {
    pvr_list_t list_type;
    struct { int alpha, shading, fog_type, culling, color_clamp, clip_mode, modifier_mode, specular, alpha2, fog_type2, color_clamp2; } gen;
    struct { int src, dst, src_enable, dst_enable, src2, dst2, src_enable2, dst_enable2; } blend;
    struct { int color, uv, modifier; } fmt;
    struct { int comparison, write; } depth;
    struct { int enable, filter, mipmap, mipmap_bias, uv_flip, uv_clamp, alpha, env, width, height, format; pvr_ptr_t base; } txr, txr2;
} pvr_poly_cxt_t;
typedef struct {
    pvr_list_t list_type;
    struct { int alpha, fog_type, culling, color_clamp, clip_mode, specular; } gen;
    struct { int src, dst, src_enable, dst_enable; } blend;
    struct { int comparison, write; } depth;
    struct { int enable, filter, mipmap, mipmap_bias, uv_flip, uv_clamp, alpha, env, width, height, format; pvr_ptr_t base; } txr;
} pvr_sprite_cxt_t;

void pvr_poly_cxt_col(pvr_poly_cxt_t *dst, pvr_list_t list);
void pvr_poly_cxt_txr(pvr_poly_cxt_t *dst, pvr_list_t list, int format, int tw, int th, pvr_ptr_t addr, pvr_filter_mode_t filter);
void pvr_poly_compile(pvr_poly_hdr_t *dst, const pvr_poly_cxt_t *src);
void pvr_sprite_cxt_col(pvr_sprite_cxt_t *dst, pvr_list_t list);
void pvr_sprite_cxt_txr(pvr_sprite_cxt_t *dst, pvr_list_t list, int format, int tw, int th, pvr_ptr_t addr, pvr_filter_mode_t filter);
void pvr_sprite_compile(pvr_sprite_hdr_t *dst, const pvr_sprite_cxt_t *src);

/* Direct rendering: each commit is one 32-byte TA record, captured for the frame (outside a capture: dropped). */
void *pvr_dr_target(void);
void pvr_dr_commit(void *record);

/* Texture memory: ordinary memory; a texture's place in the header's 21-bit address is a key the capture
   resolves back (shim/pvr_lite.c). */
pvr_ptr_t pvr_mem_malloc(size_t size);
void pvr_mem_free(pvr_ptr_t p);
void pvr_txr_load(const void *src, pvr_ptr_t dst, size_t count);
int pvr_wait_ready(void);          /* 0: the last captured frame is drawn (nothing reads textures any more) */
int pvr_wait_render_done(void);
void pvr_set_pal_format(pvr_palfmt_t format);
void pvr_set_pal_entry(uint32_t index, uint32_t value);
#define PVR_PALETTE_CFG 0x0108u
#define PVR_PALETTE_TABLE_BASE 0x1000u
uint32_t pvr_lite_get(uint32_t reg);
#define PVR_GET(reg) pvr_lite_get(reg)

/* The DcTx header (KOS utils/pvrtex file_dctex.h) and enDjinn's texture record. */
typedef struct {
    char fourcc[4];
    uint32_t chunk_size;
    uint8_t version, header_size, codebook_size, colors_used;
    uint16_t width_pixels, height_pixels;
    uint32_t pvr_type;          /* the PVR's texture words: format in the top 10 bits, size in the low 6 */
    uint8_t reserved[12];
} fDtHeader;
typedef fDtHeader dt_header_t;
/* VQ textures with a small codebook: the PVR reads the codebook from 2 KB before the indices, so the pointer moves
   back by the entries that are not there (KOS pvrtex file_dctex.h) */
static inline pvr_ptr_t fDtAdjustPVRPointer(const fDtHeader *h, pvr_ptr_t p)
{
    if (!((h->pvr_type >> 30) & 1u)) return p;
    return (pvr_ptr_t)((char *)p - 2048 + ((unsigned)h->codebook_size + 1u) * 8u);
}
typedef struct {
    dt_header_t hdr;
    uint32_t pvrformat;
    union flags {
        uint32_t raw;
        struct { uint32_t palettised : 1, twiddled : 1, compressed : 1, strided : 1, mipmapped : 1, loaded : 1,
                          palette_position : 10, palette_format : 2, num_palette_colors : 9, : 5; };
    } flags;
    uint16_t width, height;
    pvr_ptr_t ptr;
} enj_texture_info_t;
int enj_texture_load_blob(const void *data, enj_texture_info_t *texinfo);
int enj_texture_load_file(const char *filename, enj_texture_info_t *texinfo);
int enj_texture_unload(enj_texture_info_t *texinfo);

/* The capture of one frame (shim/pvr_lite.c): enj_render_list_add's callbacks run at its end, list by list. */
typedef struct pvr_lite_frame pvr_lite_frame_t;
#endif
