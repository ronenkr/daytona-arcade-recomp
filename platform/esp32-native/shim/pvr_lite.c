/* The PVR side of the native game's 2D screens on the console (include/enDjinn/pvr_lite.h).
   - The context and compile functions follow KallistiOS's pvr_prim.c (KOS License, Megan Potter et al.): the
     headers are the PVR's TA records, bit for bit.
   - Texture memory is ordinary memory (PSRAM); a texture's 21-bit address in a header is a key in a registry.
   - Palette RAM is a table of 1024 ARGB words behind PVR_GET/pvr_set_pal_entry, so render/materials/palette_ram.c
     runs unchanged.
   - Each frame (the updater's call), the callbacks of enj_render_list_add run at its end, list by list (opaque,
     punch-through, translucent; the modifier lists are the shadows, which sw3d draws itself), and their records
     are kept with a copy of the palette: pvr_draw.c draws them over the frame's picture, or alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <enDjinn/enj_enDjinn.h>
#include "pvr_lite_frame.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define LITE_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
/* texture memory 32-byte aligned: a header holds the address in 8-byte units (PSRAM blocks can be 4-aligned:
   an address one block's margin resolved was then 4 bytes off, 8 texels: the title's fuzz) */
#define TEXTURE_ALLOC(n) heap_caps_aligned_alloc(32, (n), MALLOC_CAP_SPIRAM)
#else
#define LITE_ALLOC(n) malloc(n)
#define TEXTURE_ALLOC(n) aligned_alloc(32, ((n) + 31u) & ~(size_t)31u)
#endif

/* ------------------------------------------------------------------ headers (KOS pvr_prim.c) */

static uint32_t txr_key(pvr_ptr_t p);

void pvr_poly_cxt_col(pvr_poly_cxt_t *dst, pvr_list_t list)
{
    memset(dst, 0, sizeof *dst);
    const int alpha = list > PVR_LIST_OP_MOD;
    dst->list_type = list;
    dst->fmt.color = PVR_CLRFMT_ARGBPACKED;
    dst->gen.shading = 1;
    dst->depth.comparison = PVR_DEPTHCMP_GREATER;
    dst->depth.write = PVR_DEPTHWRITE_ENABLE;
    dst->gen.culling = PVR_CULLING_CCW;
    dst->gen.alpha = alpha;
    dst->blend.src = alpha ? PVR_BLEND_SRCALPHA : PVR_BLEND_ONE;
    dst->blend.dst = alpha ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ZERO;
    dst->gen.fog_type = PVR_FOG_DISABLE;
}

void pvr_poly_cxt_txr(pvr_poly_cxt_t *dst, pvr_list_t list, int format, int tw, int th, pvr_ptr_t addr, pvr_filter_mode_t filter)
{
    pvr_poly_cxt_col(dst, list);
    const int alpha = list > PVR_LIST_OP_MOD;
    dst->txr.enable = 1;
    dst->txr.env = alpha ? PVR_TXRENV_MODULATEALPHA : PVR_TXRENV_MODULATE;
    dst->txr.filter = filter;
    dst->txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    dst->txr.width = tw; dst->txr.height = th;
    dst->txr.base = addr; dst->txr.format = format;
}

static uint32_t mode2_of(int src, int dst, int src_en, int dst_en, int fog, int clamp, int alpha)
{
    return PVR_LITE_FIELD(src, 29, 3) | PVR_LITE_FIELD(dst, 26, 3) | PVR_LITE_FIELD(src_en, 25, 1) |
           PVR_LITE_FIELD(dst_en, 24, 1) | PVR_LITE_FIELD(fog, 22, 2) | PVR_LITE_FIELD(clamp, 21, 1) | PVR_LITE_FIELD(alpha, 20, 1);
}
static uint32_t mode2_txr(int alpha, int flip, int clamp, int filter, int bias, int env, int w, int h)
{
    return PVR_LITE_FIELD(alpha, 19, 1) | PVR_LITE_FIELD(flip, 17, 2) | PVR_LITE_FIELD(clamp, 15, 2) |
           PVR_LITE_FIELD(filter, 13, 2) | PVR_LITE_FIELD(bias, 8, 4) | PVR_LITE_FIELD(env, 6, 2) |
           PVR_LITE_FIELD(__builtin_ctz((unsigned)w) - 3, 3, 3) | PVR_LITE_FIELD(__builtin_ctz((unsigned)h) - 3, 0, 3);
}

void pvr_poly_compile(pvr_poly_hdr_t *dst, const pvr_poly_cxt_t *src)
{
    dst->cmd = PVR_CMD_POLYHDR | PVR_LITE_FIELD(src->txr.enable, 3, 1) | PVR_LITE_FIELD(src->list_type, 24, 3) |
               PVR_LITE_FIELD(src->fmt.color, 4, 2) | PVR_LITE_FIELD(src->gen.shading, 1, 1) |
               PVR_LITE_FIELD(src->fmt.uv, 0, 1) | PVR_LITE_FIELD(src->gen.clip_mode, 16, 2) |
               PVR_LITE_FIELD(src->fmt.modifier, 7, 1) | PVR_LITE_FIELD(src->gen.modifier_mode, 6, 1) |
               PVR_LITE_FIELD(src->gen.specular, 2, 1);
    dst->mode1 = PVR_LITE_FIELD(src->depth.comparison, 29, 3) | PVR_LITE_FIELD(src->gen.culling, 27, 2) |
                 PVR_LITE_FIELD(src->depth.write, 26, 1) | PVR_LITE_FIELD(src->txr.enable, 25, 1);
    uint32_t mode2 = mode2_of(src->blend.src, src->blend.dst, src->blend.src_enable, src->blend.dst_enable,
                              src->gen.fog_type, src->gen.color_clamp, src->gen.alpha), mode3 = 0;
    if (src->txr.enable) {
        mode2 |= mode2_txr(src->txr.alpha, src->txr.uv_flip, src->txr.uv_clamp, src->txr.filter, src->txr.mipmap_bias,
                           src->txr.env, src->txr.width, src->txr.height);
        mode3 = PVR_LITE_FIELD(src->txr.mipmap, 31, 1) | (uint32_t)src->txr.format | txr_key(src->txr.base);
    }
    dst->mode2 = mode2; dst->mode3 = mode3;
    dst->d1 = dst->d2 = dst->d3 = dst->d4 = 0;
}

void pvr_sprite_cxt_col(pvr_sprite_cxt_t *dst, pvr_list_t list)
{
    memset(dst, 0, sizeof *dst);
    const int alpha = list > PVR_LIST_OP_MOD;
    dst->list_type = list;
    dst->depth.comparison = PVR_DEPTHCMP_GREATER;
    dst->depth.write = PVR_DEPTHWRITE_ENABLE;
    dst->gen.culling = PVR_CULLING_CCW;
    dst->gen.alpha = alpha;
    dst->blend.src = alpha ? PVR_BLEND_SRCALPHA : PVR_BLEND_ONE;
    dst->blend.dst = alpha ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ZERO;
    dst->gen.fog_type = PVR_FOG_DISABLE;
}

void pvr_sprite_cxt_txr(pvr_sprite_cxt_t *dst, pvr_list_t list, int format, int tw, int th, pvr_ptr_t addr, pvr_filter_mode_t filter)
{
    pvr_sprite_cxt_col(dst, list);
    const int alpha = list > PVR_LIST_OP_MOD;
    dst->txr.enable = 1;
    dst->txr.env = alpha ? PVR_TXRENV_MODULATEALPHA : PVR_TXRENV_MODULATE;
    dst->txr.filter = filter;
    dst->txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    dst->txr.width = tw; dst->txr.height = th;
    dst->txr.base = addr; dst->txr.format = format;
}

void pvr_sprite_compile(pvr_sprite_hdr_t *dst, const pvr_sprite_cxt_t *src)
{
    dst->cmd = PVR_CMD_SPRITE | PVR_LITE_FIELD(src->txr.enable, 3, 1) | PVR_LITE_FIELD(src->list_type, 24, 3) |
               PVR_LITE_FIELD(PVR_UVFMT_16BIT, 0, 1) | PVR_LITE_FIELD(src->gen.clip_mode, 16, 2) |
               PVR_LITE_FIELD(src->gen.specular, 2, 1);
    dst->mode1 = PVR_LITE_FIELD(src->depth.comparison, 29, 3) | PVR_LITE_FIELD(src->gen.culling, 27, 2) |
                 PVR_LITE_FIELD(src->depth.write, 26, 1) | PVR_LITE_FIELD(src->txr.enable, 25, 1);
    uint32_t mode2 = mode2_of(src->blend.src, src->blend.dst, src->blend.src_enable, src->blend.dst_enable,
                              src->gen.fog_type, src->gen.color_clamp, src->gen.alpha), mode3 = 0;
    if (src->txr.enable) {
        mode2 |= mode2_txr(src->txr.alpha, src->txr.uv_flip, src->txr.uv_clamp, src->txr.filter, src->txr.mipmap_bias,
                           src->txr.env, src->txr.width, src->txr.height);
        mode3 = PVR_LITE_FIELD(src->txr.mipmap, 31, 1) | (uint32_t)src->txr.format | txr_key(src->txr.base);
    }
    dst->mode2 = mode2; dst->mode3 = mode3;
    dst->argb = 0xffffffffu; dst->oargb = 0;
}

/* ------------------------------------------------------------------ texture memory */

/* Each block gets a range of the 21-bit address space (8-byte units, 16 MB) that no other live block has; the
   header's address field is a key there. Blocks are few (hundreds): a sorted array. */
#define TXR_BLOCKS 768
typedef struct { uint8_t *base; uint32_t bytes, key; int stored; uint32_t sum; } txr_block_t;   /* key: first 8-byte unit; stored:
                                                                                  in the flash store, not freed */
static txr_block_t blocks[TXR_BLOCKS];
static unsigned block_count;

static unsigned block_units(uint32_t bytes) { return (bytes + 2047u + 7u) / 8u + 256u; }   /* a 2 KB margin each side */

/* a block's place in key space (first fit); 0: none left */
static int block_add(uint8_t *p, size_t size, int stored)
{
    if (block_count >= TXR_BLOCKS) return 0;
    uint32_t key = 256;
    unsigned at = 0;
    for (; at < block_count; ++at) {
        if (key + block_units((uint32_t)size) <= blocks[at].key) break;
        key = blocks[at].key + block_units(blocks[at].bytes);
    }
    if (key + block_units((uint32_t)size) > (1u << 21)) return 0;
    memmove(blocks + at + 1, blocks + at, sizeof(txr_block_t) * (block_count - at));
    blocks[at] = (txr_block_t){ p, (uint32_t)size, key, stored };
    ++block_count;
    return 1;
}

pvr_ptr_t pvr_mem_malloc(size_t size)
{
    uint8_t *p = TEXTURE_ALLOC(size ? size : 1);
    if (!p) return NULL;
    if (!block_add(p, size, 0)) { free(p); return NULL; }
    return p;
}


void pvr_mem_free(pvr_ptr_t p)
{
    for (unsigned i = 0; i < block_count; ++i)
        if (blocks[i].base == p) {
            if (!blocks[i].stored) free(p);
            memmove(blocks + i, blocks + i + 1, sizeof(txr_block_t) * (block_count - i - 1));
            --block_count;
            return;
        }
}

static uint32_t block_sum(const uint8_t *p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u; return h; }
void pvr_txr_load(const void *src, pvr_ptr_t dst, size_t count)
{
    memcpy(dst, src, count);
    for (unsigned i = 0; i < block_count; ++i) if (blocks[i].base == dst) blocks[i].sum = block_sum(dst, (uint32_t)count);
}
/* diagnostics: texture blocks whose contents changed since they were loaded */
void pvr_lite_check(void)
{
    unsigned bad = 0, first = 0;
    for (unsigned i = 0; i < block_count; ++i)
        if (blocks[i].sum && !blocks[i].stored && block_sum(blocks[i].base, blocks[i].bytes) != blocks[i].sum) { if (!bad++) first = i; }
    if (bad) printf("pvr_lite: %u of %u texture blocks changed since loading (first: %u bytes at %p)\n", bad, block_count,
                    (unsigned)blocks[first].bytes, (void *)blocks[first].base);
}

/* a pointer in (or within 2 KB around) a block -> its header address field */
static uint32_t txr_key(pvr_ptr_t p)
{
    const uint8_t *q = p;
    for (unsigned i = 0; i < block_count; ++i)   /* the block that holds it, before any block's margin */
        if (q >= blocks[i].base && q < blocks[i].base + blocks[i].bytes)
            return (blocks[i].key + (uint32_t)((q - blocks[i].base) / 8)) & 0x1fffffu;
    for (unsigned i = 0; i < block_count; ++i) {
        const txr_block_t *b = blocks + i;
        if (q + 2048 >= b->base && q < b->base + b->bytes + 2048) {
            const int32_t units = (int32_t)((q - b->base) / 8);
            return (uint32_t)((int32_t)b->key + units) & 0x1fffffu;
        }
    }
    return 0;
}

const uint8_t *pvr_lite_texture(uint32_t mode3)
{
    const uint32_t key = mode3 & 0x1fffffu;
    unsigned lo = 0, hi = block_count;
    while (lo < hi) {   /* the last block whose key is <= key */
        const unsigned mid = (lo + hi) / 2;
        if (blocks[mid].key <= key) lo = mid + 1; else hi = mid;
    }
    if (!lo) return NULL;
    const txr_block_t *b = blocks + lo - 1;
    if (key >= b->key + block_units(b->bytes)) return NULL;
    return b->base + ((int32_t)key - (int32_t)b->key) * 8;
}

static uint32_t block_sum(const uint8_t *p, uint32_t n);
/* ------------------------------------------------------------------ enDjinn's texture loader */

int enj_texture_load_blob(const void *data, enj_texture_info_t *texinfo)
{
    if (!data || !texinfo) return 0;
    enj_texture_info_t t = { 0 };
    memcpy(&t.hdr, data, sizeof t.hdr);
    if (memcmp(t.hdr.fourcc, "DcTx", 4)) return 0;
    const size_t meta = ((size_t)1 + t.hdr.header_size) << 5;
    if (t.hdr.chunk_size <= meta) return 0;
    const size_t bytes = t.hdr.chunk_size - meta;
    const unsigned fmt = (t.hdr.pvr_type >> 27) & 7u;
    t.flags.palettised = fmt == 5 || fmt == 6;
    t.flags.compressed = (t.hdr.pvr_type >> 30) & 1u;
    t.flags.mipmapped = t.hdr.pvr_type >> 31;
    t.flags.twiddled = t.flags.palettised || !((t.hdr.pvr_type >> 26) & 1u);
    t.flags.num_palette_colors = t.flags.palettised ? (unsigned)t.hdr.colors_used + 1u : 0u;
    t.width = (uint16_t)(8u << ((t.hdr.pvr_type >> 3) & 7u));   /* the low 6 bits: the PVR's size field */
    t.height = (uint16_t)(8u << (t.hdr.pvr_type & 7u));
    t.pvrformat = t.hdr.pvr_type & 0xffc00000u;
    /* the same texture data in the console's flash store: used where it is */
    const uint8_t *stored = pvr_lite_store_lookup ? pvr_lite_store_lookup((const uint8_t *)data + meta, (uint32_t)bytes) : NULL;
    if (stored && block_add((uint8_t *)stored, bytes, 1)) t.ptr = (pvr_ptr_t)stored;
    else {
        t.ptr = pvr_mem_malloc(bytes);
        if (!t.ptr) { printf("pvr_lite: no memory for a %u-byte texture\n", (unsigned)bytes); return 0; }
        pvr_txr_load((const uint8_t *)data + meta, t.ptr, bytes);
    }
    t.flags.loaded = 1;
    *texinfo = t;
#ifdef PVR_LITE_TRACE
    printf("pvr_lite: texture %ux%u fmt %08lx %u bytes sum %08lx%s\n", t.width, t.height, (unsigned long)t.hdr.pvr_type,
           (unsigned)bytes, (unsigned long)block_sum(t.ptr, (uint32_t)bytes), stored ? " (stored)" : "");
#endif
    return 1;
}

int enj_texture_load_file(const char *filename, enj_texture_info_t *texinfo)
{
    FILE *f = filename ? fopen(filename, "rb") : NULL;
    if (!f) { printf("pvr_lite: cannot open %s\n", filename ? filename : "(null)"); return 0; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size > 0 ? LITE_ALLOC((size_t)size) : NULL;
    const int ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size && enj_texture_load_blob(buf, texinfo);
    free(buf);
    fclose(f);
    return ok;
}

int enj_texture_unload(enj_texture_info_t *texinfo)
{
    if (!texinfo || !texinfo->ptr) return 0;
    pvr_wait_ready();   /* nothing may still draw from it */
    pvr_mem_free(texinfo->ptr);
    texinfo->ptr = NULL;
    texinfo->flags.loaded = 0;
    return 1;
}

/* ------------------------------------------------------------------ palette RAM */

static uint32_t palette_ram[1024];
static uint32_t palette_cfg = PVR_PAL_ARGB8888;

void pvr_set_pal_format(pvr_palfmt_t format) { palette_cfg = (uint32_t)format & 3u; }
void pvr_set_pal_entry(uint32_t index, uint32_t value) { palette_ram[index & 1023u] = value; }
uint32_t pvr_lite_get(uint32_t reg)
{
    if (reg == PVR_PALETTE_CFG) return palette_cfg;
    if (reg >= PVR_PALETTE_TABLE_BASE && reg < PVR_PALETTE_TABLE_BASE + 4096u) return palette_ram[(reg - PVR_PALETTE_TABLE_BASE) / 4u];
    return 0;
}

/* ------------------------------------------------------------------ the frame's capture */

#define CALLBACKS 64
static struct { int list; void (*callback)(void *); void *data; } queued[CALLBACKS];
static unsigned queued_count;
static pvr_lite_frame_t frames[2];
static pvr_lite_frame_t *capturing;   /* the frame pvr_dr_commit appends to, NULL outside a capture */
static uint32_t scratch[8];
static unsigned capture_index;
void (*pvr_lite_wait_hook)(void);
const uint8_t *(*pvr_lite_store_lookup)(const uint8_t *data, uint32_t bytes);

void enj_render_list_add(int list, void (*callback)(void *), void *data)
{
    if (list == PVR_LIST_OP_MOD || list == PVR_LIST_TR_MOD || queued_count >= CALLBACKS) return;
    queued[queued_count].list = list; queued[queued_count].callback = callback; queued[queued_count].data = data;
    ++queued_count;
}

void *pvr_dr_target(void) { return scratch; }

static void append(const uint32_t record[8])
{
    pvr_lite_frame_t *f = capturing;
    if (f->count == f->cap) {
        const unsigned cap = f->cap ? f->cap * 2u : 1024u;
        uint32_t *more = LITE_ALLOC(sizeof(uint32_t) * 8u * cap);
        if (!more) return;
        if (f->records) { memcpy(more, f->records, sizeof(uint32_t) * 8u * f->count); free(f->records); }
        f->records = more; f->cap = cap;
    }
    memcpy(f->records + 8u * f->count++, record, 32);
}

void pvr_dr_commit(void *record) { if (capturing) append(record); }

int pvr_lite_pending(void) { return queued_count != 0; }

/* Runs the queued callbacks into the next capture buffer (opaque, punch-through, translucent), each list after
   a marker record (word 0 zero, word 1 the list); keeps the palette. NULL when nothing was queued. */
pvr_lite_frame_t *pvr_lite_capture(void)
{
    if (!queued_count) return NULL;
    pvr_lite_frame_t *f = frames + (capture_index++ & 1u);
    f->count = 0;
    capturing = f;
    static const int order[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
    for (unsigned l = 0; l < 3; ++l) {
        const uint32_t marker[8] = { 0, (uint32_t)order[l] };
        append(marker);
        for (unsigned i = 0; i < queued_count; ++i) if (queued[i].list == order[l]) queued[i].callback(queued[i].data);
    }
    capturing = NULL;
    memcpy(f->palette, palette_ram, sizeof palette_ram);
    queued_count = 0;
    return f;
}

void pvr_lite_discard(void) { queued_count = 0; }

/* At start-up, before any course is loaded: both capture buffers at their working size (2,048 records, 64 KB
   each), so that no later growth lands between the blocks of a course. */
void pvr_lite_reserve(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (!frames[i].records) {
            frames[i].records = LITE_ALLOC(sizeof(uint32_t) * 8u * 2048u);
            frames[i].cap = frames[i].records ? 2048u : 0u;
        }
}

int pvr_wait_ready(void) { if (pvr_lite_wait_hook) pvr_lite_wait_hook(); return 0; }
int pvr_wait_render_done(void) { return pvr_wait_ready(); }
