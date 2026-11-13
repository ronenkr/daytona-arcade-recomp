/* The EspRacer software renderer (sw3d.h). Version 1: correct before fast.
 *
 * Load: the car and world packs and their DNT6 material packs, as code/render/scene/practice_assets.c
 * loads them for the Dreamcast, without the VRAM upload, the FTRV gates and the stream tables.
 * Frame: code/render/scene/practice.c prepares the instances and calls nm_render_enqueue_objects and
 * nm_sky_enqueue, which draw here directly (nothing is queued).
 *
 * Rules (from the Dreamcast path):
 * - projection per instance: x' = cx + x*F/z, y' = cy - y*F/z in the 496x384 logical view, cx = center[0],
 *   cy = 384 - center[1] (model_math.h instance_view); near plane z = 0.01; faces with a corner beyond
 *   the sky (1/z <= NM_SKY_DEPTH) are dropped (model_submission.h);
 * - world faces carry their luma (NM_PACK_BAKED_LIGHT): front when the supplied normal faces the eye
 *   (n.p2 >= 0 in view space), else the back luma for a two-sided face (attribute bit 17) or culled;
 *   car faces are lit with face_light (model_math.h);
 * - binding = nm_material_find(pack, material.header, luma); a solid binding is its colour, a textured one
 *   the palette entry times the binding's vertex colour; texel alpha < 128 is a hole (punch-through);
 * - depth: 1/z, times the road-surface scale for surface-class materials and 1+layer/1024 for decals,
 *   GEQUAL (larger is nearer).
 * Not yet: mip levels (base level only), bilinear filtering, translucent smoke, shadows, the stipple
 * fence (material stage 4), level-of-detail groups beyond level 0, live specular. */
#include "sw3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "practice_scene.h"
#include "material_pack.h"
#include "meshpack.h"
#include "course_assets.h"
#include "pose.h"
#include "render/models/model_math.h"
#include "render/scene/practice.h"
#ifdef DAYTONA_UI
#include "render/ui/finish_render.h"
#include "render/ui/result_render.h"
#include "render/ui/result_labels.h"
#include "render/materials/result_materials.h"
#endif
#include "render/scene/track_surface.h"
#include "render/scene/sky.h"
#include "render/scene/damage_bodies.h"
#include "render/materials/palette_ram.h"
#include "render/scene/slot_glass.h"
#include "render/ui/finish_render.h"
#include "render/ui/hud_render.h"
#include "intro_drive_data.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#define CYCLES() esp_cpu_get_cycle_count()
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
#else
#include <time.h>
#define BIG_ALLOC(n) malloc(n)
#define CYCLES() 0u
static uint32_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000); }
#endif

/* The pipeline (console, unless SW3D_NO_PIPELINE): core 1 draws picture N (all rows) while core 0 runs the
   ticks and builds the list of N+1; the lists, their order and paints are double-buffered. Otherwise (and on
   the host) both cores build, then both draw (even and odd rows), one after the other. */
#if !defined(SW3D_NO_PIPELINE) && defined(ESP_PLATFORM)
#define PIPE 1
#else
#define PIPE 0
#endif
/* ALL_ROWS: one pass draws every row (PIPE; SW3D_ALL_ROWS checks that drawing on the host) */
#if PIPE || defined(SW3D_ALL_ROWS)
#define ALL_ROWS 1
#else
#define ALL_ROWS 0
#endif
#define W SW3D_W
#define H SW3D_VIEW_H   /* the rows the renderer draws; G.pixels points at the view's first row */
#define NEAR_Z 0.01f
/* The draw distance (view-space depth) beyond which meshes and groups of 8 world faces are not drawn. The
   arcade draws to the sky (1/NM_SKY_DEPTH = 10000); a shorter one is detail traded for speed (porter guide 6.0). */
#ifndef SW3D_FAR
#define SW3D_FAR 10000.0f
#endif
#define XS ((float)W / 496.0f)
#define YS ((float)H / 384.0f)

void (*sw3d_frame_done)(const uint16_t *pixels, unsigned frame);
/* The 2D layer over each picture (the game's screens: shim/frame_ui.c), after the fill and before
   sw3d_frame_done; tag: the picture's number (sw3d_posted when it was handed to drawing), 0 for a picture of
   the 2D layer alone (sw3d_present_alone). */
void (*sw3d_overlay)(uint16_t *view, uint16_t *depth, uint32_t tag);
volatile uint32_t sw3d_posted;
int sw3d_defer_done;   /* without the pipeline: sw3d_finish presents (after the frame's 2D layer is captured) */
static int g_done_pending;
static uint32_t g_done_tag;
int (*sw3d_frame_wanted)(unsigned frame);

#define MAX_LEVELS 11
/* A texture as the renderer reads it: PAL4 in plain rows (two texels a byte, the even one in the low nibble),
   level 0 the full size, then its mip levels (square textures with a chain only). Converted from the
   Dreamcast's twiddled layout at load: an address is then a shift and an OR, not two table lookups. */
typedef struct {
    uint8_t *data;
    uint8_t levels;
    uint8_t in_store;           /* data is in the flash texture store (not freed) */
    uint32_t offset[MAX_LEVELS];
} texture_t;

typedef struct {
    nm_material_pack_t pack;
    texture_t *texture;         /* per resource */
} materials_t;

static struct {
    nm_practice_scene_t *scene;
    materials_t car, world;
    uint8_t *world_skip;        /* unused */
    uint16_t *face_lod;         /* per world face: its level-of-detail group + 1 (0: always drawn) */
    uint8_t *face_level;        /* per world face: the level of its range in that group (0xff: never drawn) */
    uint16_t *world_binding;    /* per world face, front and back: binding index + 1 (0: not looked up yet) */
    uint16_t *pixels;           /* the view's first row */
    uint16_t *picture;          /* the whole picture */
    uint16_t *depth;            /* depth16 (below): 0 = nothing drawn yet (the sky goes there) */
    float road_depth;
    unsigned frame;
    /* sky */
    uint8_t *sky_pages;         /* 4 VQ pages of NM_SKY_TEXTURE_BYTES, or NULL */
    uint32_t sky_fill[2];
    unsigned sky_rows;
    sw3d_stats_t stats;
    /* the frame's lists */
    unsigned tri_count, car_part_count, lod_count;
    uint32_t last_done_us;
    uint16_t sky_heading, sky_scroll;
} G = { .road_depth = NM_ROAD_DEPTH_SCALE };

const sw3d_stats_t *sw3d_stats(void) { return &G.stats; }
void sw3d_set_target(uint16_t *pixels) { G.pixels = pixels + SW3D_VIEW_Y * W; G.picture = pixels; }

static inline uint16_t rgb565(uint32_t argb)
{
    return (uint16_t)(((argb >> 8) & 0xf800u) | ((argb >> 5) & 0x07e0u) | ((argb >> 3) & 0x001fu));
}

/* ------------------------------------------------------------------ twiddle tables */

/* tw_x[v] / tw_y[v]: the interleaved bits of a coordinate below 1024 (x on odd bits, y on even). */
static uint32_t tw_x[1024], tw_y[1024];
static void twiddle_init(void)
{
    for (unsigned v = 0; v < 1024; ++v) {
        uint32_t x = 0, y = 0;
        for (unsigned b = 0; b < 10; ++b) { y |= ((v >> b) & 1u) << (2 * b); x |= ((v >> b) & 1u) << (2 * b + 1); }
        tw_x[v] = x; tw_y[v] = y;
    }
}

/* ------------------------------------------------------------------ materials */

/* Twiddled PAL4 texel (tx, ty) of a w x h level (side = min(w, h); blocks of side x side in a row or column). */
static unsigned twiddled_texel(const uint8_t *src, int side, int tx, int ty)
{
    const int sshift = __builtin_ctz((unsigned)side);
    const uint32_t idx = (uint32_t)(((tx >> sshift) + (ty >> sshift)) << (2 * sshift)) + tw_x[tx & (side - 1)] + tw_y[ty & (side - 1)];
    const uint8_t byte = src[idx >> 1];
    return idx & 1 ? byte >> 4 : byte & 15u;
}

static inline uint32_t dctx_mode(const uint8_t *file)
{
    return (uint32_t)file[16] | (uint32_t)file[17] << 8 | (uint32_t)file[18] << 16 | (uint32_t)file[19] << 24;
}

/* The levels and their offsets of a converted texture (linear PAL4, base level first); its size in bytes. */
static size_t texture_layout(texture_t *t, uint32_t mode, int w, int h)
{
    const int mips = (mode & 0x80000000u) && w == h;
    t->levels = 1;
    t->in_store = 0;
    if (mips) for (int s = w; s > 1 && t->levels < MAX_LEVELS; s >>= 1) ++t->levels;
    size_t bytes = 0;
    for (unsigned l = 0; l < t->levels; ++l) {
        t->offset[l] = (uint32_t)bytes;
        const int lw = w >> l, lh = h >> l;
        bytes += ((size_t)lw * lh + 1) / 2;
    }
    return bytes;
}

static int texture_convert(texture_t *t, const uint8_t *file, int w, int h)
{
    const uint32_t mode = dctx_mode(file);
    const int mips = (mode & 0x80000000u) && w == h;
    const size_t bytes = texture_layout(t, mode, w, h);
    t->data = BIG_ALLOC(bytes);
    if (!t->data) return 0;
    memset(t->data, 0, bytes);
    for (unsigned l = 0; l < t->levels; ++l) {
        const int lw = w >> l, lh = h >> l, side = lw < lh ? lw : lh;
        /* the source level: the base is the last one of a chain; level of side s starts at 2 + sum t*t/2 */
        size_t at = 32;
        if (mips) { at += 2; for (int s = 2; s < lw; s *= 2) at += (size_t)s * s / 2; if (lw == 1) at = 32; }
        const uint8_t *src = file + at;
        uint8_t *dst = t->data + t->offset[l];
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                const unsigned v = lw == 1 && lh == 1 ? (src[0] & 15u) : twiddled_texel(src, side, x, y);
                const uint32_t i = (uint32_t)y * (uint32_t)lw + (uint32_t)x;
                dst[i >> 1] |= (uint8_t)(i & 1 ? v << 4 : v);
            }
    }
    return 1;
}

/* ------------------------------------------------------------------ the flash texture store (console) */

/* The converted textures of the car pack and the two larger courses do not fit in PSRAM beside those courses'
   meshes and tables (Expert needs about 3 MB more than Beginner): the console keeps them in a flash data
   partition (EspRacer partitions.csv: "daytona", subtype 0x40), read through the memory map like PSRAM. The game
   fills it from its SD folder once (sw3d_store_prepare: the car textures, then Expert's, then Advanced's until
   it is full); a pack's signature (its size and its texture records) tells when a file changed and the store
   is filled again. Beginner's textures stay in PSRAM. */
static const char *const store_packs[] = { "car.dnt", "course1-world.dnt", "course2-world.dnt" };
#define STORE_PACKS (sizeof store_packs / sizeof store_packs[0])
#define STORE_MAGIC 0x35585444u   /* "DTX5": car, Expert, the 2D screens, then Advanced (DTX4: Advanced before the 2D
                                     screens, which then took PSRAM between the courses' blocks); with the 2D screens' textures (DTX1, DTX2: filled before the twiddle
                                     tables were set up: flat colours; DTX3: without the 2D screens) */
/* After the packs: the texture data of the 2D screens' DcTx files (raw, as the PVR would hold them), found by
   their contents (hash and size; shim/pvr_lite.c's loader asks), in this order until the store is full. */
static const char *const store_ui_dirs[] = { "select", "attract-2d", "hud", "labels", "result", "finish", "name-labels", "podium" };
#define STORE_UI_DIRS (sizeof store_ui_dirs / sizeof store_ui_dirs[0])
#define STORE_META 0xffffu        /* an entry's resource: the pack's metadata block (records, bindings, palettes) */
static const uint8_t store_meta_packs[] = { 1, 1, 0 };   /* whose metadata goes to the store (the car's, Expert's) */
#define STORE_DIR_BYTES 0x8000u   /* the directory; the textures follow, 64-byte aligned */
typedef struct { uint16_t pack, resource; uint32_t offset, mode; uint16_t width, height; } store_entry_t;
typedef struct { uint32_t magic, count, used, sig[STORE_PACKS], ui_sig; } store_head_t;   /* UI entries: pack STORE_PACKS */
#define STORE_MAX_ENTRIES ((STORE_DIR_BYTES - sizeof(store_head_t)) / sizeof(store_entry_t))
static const uint8_t *g_store;    /* the mapped partition, NULL: none (everything in PSRAM) */
static unsigned g_store_first[STORE_PACKS + 1], g_store_end[STORE_PACKS + 1];   /* each pack's entries; [STORE_PACKS]: the UI's */

static int store_pack_of(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (unsigned i = 0; i < STORE_PACKS; ++i) if (!strcmp(name, store_packs[i])) return (int)i;
    return -1;
}

/* a pack's signature: its whole metadata block (texture records, bindings, modulation, palettes) */
static uint32_t store_signature(const nm_material_pack_t *pack)
{
    uint32_t h = 2166136261u ^ pack->resources ^ pack->metadata;
    const uint8_t *p = (const uint8_t *)pack->data;
    for (size_t i = 0; i < pack->metadata; ++i) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1u;
}

/* The store's copy of a pack's metadata block, or NULL. */
static const uint8_t *store_metadata(int pack, uint32_t bytes)
{
    if (!g_store || pack < 0) return NULL;
    const store_entry_t *dir = (const store_entry_t *)(g_store + sizeof(store_head_t));
    for (unsigned i = g_store_first[pack]; i < g_store_end[pack]; ++i)
        if (dir[i].resource == STORE_META) return dir[i].mode == bytes ? g_store + dir[i].offset : NULL;
    return NULL;
}

/* After nm_material_pack_open_cpu: the pack's tables in the store's copy, its PSRAM block freed. */
static void store_use_metadata(int store_pack, nm_material_pack_t *pack)
{
    const uint8_t *meta = store_metadata(store_pack, pack->metadata), *base = pack->data;
    if (!meta || !base) return;
#define REBASE(type, field) if (pack->field) pack->field = (type)(meta + ((const uint8_t *)pack->field - base))
    REBASE(nm_texture_record_t *, textures);
    REBASE(nm_material_binding_t *, bindings);
    REBASE(uint32_t *, modulation);
    REBASE(const uint8_t *, palette_index);
    REBASE(const uint32_t *, palettes);
#undef REBASE
    free(pack->data);
    pack->data = (void *)meta;   /* the lookups (nm_material_find) want it set; materials_close clears it */
}

/* the pack's tables are the store's (not to be freed) */
static int pack_in_store(const nm_material_pack_t *pack)
{
    return g_store && pack->data && (const uint8_t *)pack->data >= g_store && (const uint8_t *)pack->data < g_store + 0x1000000u;
}

/* The store's textures of one pack resource: 1 with t laid out and pointing into flash. */
static int store_texture(int pack, unsigned resource, unsigned w, unsigned h, texture_t *t)
{
    if (!g_store || pack < 0) return 0;
    const store_entry_t *dir = (const store_entry_t *)(g_store + sizeof(store_head_t));
    for (unsigned i = g_store_first[pack]; i < g_store_end[pack]; ++i) {
        const store_entry_t *e = dir + i;
        if (e->resource != resource) continue;
        if (e->width != w || e->height != h) return 0;
        texture_layout(t, e->mode, (int)w, (int)h);
        t->data = (uint8_t *)(g_store + e->offset);
        t->in_store = 1;
        return 1;
    }
    return 0;
}

void (*sw3d_store_progress)(unsigned done, unsigned total);
#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include <dirent.h>
#include <sys/stat.h>
static esp_partition_mmap_handle_t g_store_handle;
static const esp_partition_t *g_store_part;

static int store_map(void)
{
    const void *p = NULL;
    if (esp_partition_mmap(g_store_part, 0, g_store_part->size, ESP_PARTITION_MMAP_DATA, &p, &g_store_handle) != ESP_OK) return 0;
    g_store = p;
    return 1;
}

static int store_valid(const uint32_t sig[STORE_PACKS], uint32_t ui_sig)
{
    const store_head_t *h = (const store_head_t *)g_store;
    if (h->magic != STORE_MAGIC || h->count > STORE_MAX_ENTRIES || h->used > g_store_part->size || h->ui_sig != ui_sig) return 0;
    for (unsigned i = 0; i < STORE_PACKS; ++i) if (h->sig[i] != sig[i]) return 0;
    const store_entry_t *dir = (const store_entry_t *)(g_store + sizeof(store_head_t));
    for (unsigned p = 0; p <= STORE_PACKS; ++p) { g_store_first[p] = h->count; g_store_end[p] = 0; }
    for (unsigned k = 0; k < h->count; ++k) {   /* each pack's entries are consecutive */
        const unsigned p = dir[k].pack <= STORE_PACKS ? dir[k].pack : STORE_PACKS;
        if (k < g_store_first[p]) g_store_first[p] = k;
        g_store_end[p] = k + 1;
    }
    for (unsigned p = 0; p <= STORE_PACKS; ++p) if (g_store_first[p] > g_store_end[p]) g_store_first[p] = g_store_end[p] = 0;
    return 1;
}

/* Fill the partition from the packs (once; minutes at most). 1 when the store is usable afterwards. */
uint32_t sw3d_blob_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the 2D screens' folders as they are on the card (names and sizes): when it changes, the store is filled again */
static uint32_t store_ui_signature(const char *root)
{
    uint32_t h = 2166136261u;
    for (unsigned d = 0; d < STORE_UI_DIRS; ++d) {
        char path[256];
        snprintf(path, sizeof path, "%s%s", root, store_ui_dirs[d]);
        DIR *dp = opendir(path);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            char file_path[300];
            snprintf(file_path, sizeof file_path, "%s/%s", path, de->d_name);
            struct stat st;
            const uint32_t size = stat(file_path, &st) == 0 ? (uint32_t)st.st_size : 0u;
            for (const char *c = de->d_name; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
            h = (h ^ size) * 16777619u;
        }
        closedir(dp);
    }
    return h;
}

/* The store's copy of a 2D screen's texture data with this hash and size, or NULL. */
const uint8_t *sw3d_store_blob(uint32_t hash, uint32_t bytes)
{
    if (!g_store) return NULL;
    const store_entry_t *dir = (const store_entry_t *)(g_store + sizeof(store_head_t));
    for (unsigned i = g_store_first[STORE_PACKS]; i < g_store_end[STORE_PACKS]; ++i)
        if (dir[i].mode == bytes && dir[i].width == (uint16_t)(hash >> 16) && dir[i].height == (uint16_t)hash)
            return g_store + dir[i].offset;
    return NULL;
}

/* The 2D screens' DcTx files into the store after the entries so far; 1 when it filled up. */
static int store_fill_ui(const char *root, store_head_t *head, store_entry_t *dir, unsigned *ui_files)
{
    int full = 0;
    for (unsigned d = 0; d < STORE_UI_DIRS && !full; ++d) {
        char path[256];
        snprintf(path, sizeof path, "%s%s", root, store_ui_dirs[d]);
        DIR *dp = opendir(path);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            const size_t n = strlen(de->d_name);
            if (n < 4 || strcmp(de->d_name + n - 3, ".dt")) continue;
            char file_path[300];
            snprintf(file_path, sizeof file_path, "%s/%s", path, de->d_name);
            FILE *f = fopen(file_path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *blob = size > 32 ? malloc((size_t)size) : NULL;
            const int ok = blob && fread(blob, 1, (size_t)size, f) == (size_t)size && !memcmp(blob, "DcTx", 4);
            fclose(f);
            if (ok) {
                const size_t meta = ((size_t)1 + blob[9]) << 5;
                const uint32_t bytes = (uint32_t)((size_t)size - meta), hash = sw3d_blob_hash(blob + meta, bytes);
                if (meta < (size_t)size && head->used + bytes <= g_store_part->size && head->count < STORE_MAX_ENTRIES &&
                    esp_partition_write(g_store_part, head->used, blob + meta, bytes) == ESP_OK) {
                    dir[head->count++] = (store_entry_t){ (uint16_t)STORE_PACKS, 0, head->used, bytes, (uint16_t)(hash >> 16), (uint16_t)hash };
                    head->used = (head->used + bytes + 63u) & ~63u;
                    ++*ui_files;
                } else if (head->used + bytes > g_store_part->size) full = 1;
            }
            free(blob);
            if (full) break;
        }
        closedir(dp);
    }
    return full;
}

static int store_fill(const char *root, const uint32_t sig[STORE_PACKS])
{
    printf("sw3d: filling the flash texture store (%lu bytes) from the card\n", (unsigned long)g_store_part->size);
    esp_partition_munmap(g_store_handle); g_store = NULL;
    if (esp_partition_erase_range(g_store_part, 0, g_store_part->size) != ESP_OK) return 0;
    store_entry_t *dir = BIG_ALLOC(sizeof(store_entry_t) * STORE_MAX_ENTRIES);   /* PSRAM: internal RAM is the depth buffer's */
    if (!dir) return 0;
    store_head_t head = { 0, 0, STORE_DIR_BYTES, { 0 } };
    unsigned done = 0, total = 0;
    nm_material_pack_t pack;
    for (unsigned p = 0; p < STORE_PACKS; ++p) {
        char path[256];
        snprintf(path, sizeof path, "%s%s", root, store_packs[p]);
        memset(&pack, 0, sizeof pack);
        if (nm_material_pack_open_cpu(&pack, path)) {
            for (unsigned r = 0; r < pack.resources; ++r) total += pack.textures[r].size;
            nm_material_pack_close(&pack);
        }
    }
    int full = 0;
    unsigned ui_files = 0;
    head.ui_sig = store_ui_signature(root);
    static const unsigned order[] = { 0, 1, STORE_PACKS, 2 };   /* car, Expert, the 2D screens, Advanced */
    for (unsigned o = 0; o < sizeof order / sizeof order[0] && !full; ++o) {
        const unsigned p = order[o];
        if (p == STORE_PACKS) { full = store_fill_ui(root, &head, dir, &ui_files); continue; }
        char path[256];
        snprintf(path, sizeof path, "%s%s", root, store_packs[p]);
        memset(&pack, 0, sizeof pack);
        if (!nm_material_pack_open_cpu(&pack, path)) { printf("sw3d: store: cannot open %s\n", path); continue; }
        head.sig[p] = sig[p];
        if (store_meta_packs[p]) {
            if (head.used + pack.metadata > g_store_part->size || head.count >= STORE_MAX_ENTRIES ||
                esp_partition_write(g_store_part, head.used, pack.data, pack.metadata) != ESP_OK) {
                nm_material_pack_close(&pack);
                break;
            }
            dir[head.count++] = (store_entry_t){ (uint16_t)p, STORE_META, head.used, pack.metadata, 0, 0 };
            head.used = (head.used + pack.metadata + 63u) & ~63u;
        }
        size_t largest = 0;
        for (unsigned r = 0; r < pack.resources; ++r) if (pack.textures[r].size > largest) largest = pack.textures[r].size;
        uint8_t *file = malloc(largest);
        for (unsigned r = 0; file && r < pack.resources; ++r) {
            const nm_texture_record_t *rec = &pack.textures[r];
            texture_t t;
            if (!nm_material_texture_read(&pack, r, file, rec->size) || !texture_convert(&t, file, rec->width, rec->height)) {
                printf("sw3d: store: %s resource %u not read\n", path, r);
                continue;
            }
            texture_t lay;
            const size_t bytes = texture_layout(&lay, dctx_mode(file), rec->width, rec->height);
            if (head.used + bytes > g_store_part->size || head.count >= STORE_MAX_ENTRIES) { free(t.data); full = 1; break; }
            const esp_err_t we = esp_partition_write(g_store_part, head.used, t.data, bytes);
            if (we != ESP_OK) { printf("sw3d: store: write failed (%d)\n", (int)we); free(t.data); full = 1; break; }
            dir[head.count++] = (store_entry_t){ (uint16_t)p, (uint16_t)r, head.used, dctx_mode(file), rec->width, rec->height };
            head.used = (head.used + (uint32_t)bytes + 63u) & ~63u;
            free(t.data);
            done += rec->size;
            if (sw3d_store_progress) sw3d_store_progress(done, total);
        }
        free(file);
        nm_material_pack_close(&pack);
    }
    printf("sw3d: flash store: %u files of the 2D screens\n", ui_files);
    /* the directory, then the head with its magic last: an interrupted fill is not taken for a store */
    const int dir_ok = esp_partition_write(g_store_part, sizeof head, dir, sizeof(store_entry_t) * head.count) == ESP_OK;
    free(dir);
    if (!dir_ok) return 0;
    head.magic = STORE_MAGIC;
    if (esp_partition_write(g_store_part, 0, &head, sizeof head) != ESP_OK) return 0;
    printf("sw3d: flash texture store: %u textures, %lu bytes%s\n", (unsigned)head.count, (unsigned long)head.used,
           full ? " (full: the rest stays in PSRAM)" : "");
    return store_map();
}

static void tables_init(void);
int sw3d_store_prepare(const char *root)
{
    tables_init();   /* texture_convert needs the twiddle tables */
#ifdef SW3D_NO_STORE   /* check: everything in PSRAM (Beginner only) */
    (void)root; return 0;
#endif
    g_store_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "daytona");
    if (!g_store_part) { printf("sw3d: no flash texture store partition: textures in PSRAM\n"); return 0; }
    if (!store_map()) return 0;
    uint32_t sig[STORE_PACKS] = { 0 };
    nm_material_pack_t pack;
    for (unsigned p = 0; p < STORE_PACKS; ++p) {
        char path[256];
        snprintf(path, sizeof path, "%s%s", root, store_packs[p]);
        memset(&pack, 0, sizeof pack);
        memset(&pack, 0, sizeof pack);
        if (nm_material_pack_open_cpu(&pack, path)) { sig[p] = store_signature(&pack); nm_material_pack_close(&pack); }
    }
    const uint32_t ui_sig = store_ui_signature(root);
    if (store_valid(sig, ui_sig) || (store_fill(root, sig) && store_valid(sig, ui_sig))) {
        const store_head_t *h = (const store_head_t *)g_store;
        printf("sw3d: flash texture store: %u textures, %lu bytes\n", (unsigned)h->count, (unsigned long)h->used);
        return 1;
    }
    printf("sw3d: flash texture store not usable: textures in PSRAM\n");
    g_store = NULL;
    return 0;
}
#else
int sw3d_store_prepare(const char *root) { (void)root; return 0; }
#endif

/* A DNT6 pack whose metadata block is in the store: opened from its 32-byte header, the tables in flash (no
   PSRAM copy of the block first: the car's is 1.28 MB, needed in one piece at every course load). Else the
   Dreamcast's loader. */
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int materials_pack_open(nm_material_pack_t *pack, const char *path)
{
    const int sp = store_pack_of(path);
    if (g_store && sp >= 0) {
        uint8_t h[32];
        FILE *f = fopen(path, "rb");
        if (f && fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "DNT6", 4)) {
            const uint32_t resources = le32(h + 8), count = le32(h + 12), palette_count = le32(h + 24);
            const uint32_t metadata = (resources + count) * 16u + count * 4u + ((count + 3u) & ~3u) + palette_count * 68u;
            const uint8_t *meta = store_metadata(sp, metadata);
            if (meta) {
                memset(pack, 0, sizeof *pack);
                setvbuf(f, NULL, _IOFBF, 16384);
                pack->file = f;
                pack->data = (void *)meta;   /* the lookups (nm_material_find) want it set; materials_close clears it */
                pack->resources = resources; pack->count = count; pack->payload = le32(h + 16); pack->staging = le32(h + 20);
                pack->palette_count = palette_count; pack->palette_flags = le32(h + 28); pack->metadata = metadata;
                pack->textures = (nm_texture_record_t *)meta;
                pack->bindings = (nm_material_binding_t *)(pack->textures + resources);
                pack->modulation = (uint32_t *)(pack->bindings + count);
                pack->palette_index = (const uint8_t *)(pack->modulation + count);
                pack->palettes = (const uint32_t *)(pack->palette_index + ((count + 3u) & ~3u));
                return 1;
            }
        }
        if (f) fclose(f);
    }
    if (!nm_material_pack_open_cpu(pack, path)) return 0;
    store_use_metadata(sp, pack);
    return 1;
}

static int materials_open(materials_t *m, const char *path)
{
    if (!materials_pack_open(&m->pack, path)) { printf("sw3d: cannot open %s\n", path); return 0; }
    m->texture = calloc(m->pack.resources ? m->pack.resources : 1, sizeof(*m->texture));
    if (!m->texture) return 0;
    size_t total = 0, largest = 0, stored = 0;
    const int store_pack = store_pack_of(path);
    for (unsigned r = 0; r < m->pack.resources; ++r) if (m->pack.textures[r].size > largest) largest = m->pack.textures[r].size;
    uint8_t *file = malloc(largest);
    if (!file) return 0;
    for (unsigned r = 0; r < m->pack.resources; ++r) {
        const nm_texture_record_t *rec = &m->pack.textures[r];
        if (store_texture(store_pack, r, rec->width, rec->height, m->texture + r)) { stored += rec->size; continue; }
        if (!nm_material_texture_read(&m->pack, r, file, rec->size) || !texture_convert(m->texture + r, file, rec->width, rec->height)) {
            printf("sw3d: %s resource %u (%lu bytes) not read\n", path, r, (unsigned long)rec->size);
            free(file);
            return 0;
        }
        total += rec->size;
    }
    free(file);
    printf("sw3d: %s: %u textures, %lu bytes in PSRAM, %lu in flash; %u bindings, %u palettes\n", path, m->pack.resources,
           (unsigned long)total, (unsigned long)stored, m->pack.count, m->pack.palette_count);
    return 1;
}

static void materials_close(materials_t *m)
{
    if (m->texture) for (unsigned r = 0; r < m->pack.resources; ++r) if (!m->texture[r].in_store) free(m->texture[r].data);
    if (pack_in_store(&m->pack)) m->pack.data = NULL;   /* the store's tables: nothing to free */
    free(m->texture); m->texture = NULL;
    nm_material_pack_close(&m->pack);
}

/* What a binding draws with: a solid colour, or a texture level with a 16-entry RGB565 palette. */
typedef struct {
    uint16_t pal[16];           /* RGB565; a solid colour is pal[0] */
    const texture_t *texture;   /* NULL for a solid colour */
    uint16_t u_origin, v_origin;   /* crop origin in texels */
    uint16_t opaque;            /* bit i: palette entry i is drawn */
    uint8_t wshift, hshift;     /* level 0 size as shifts */
    uint8_t mirror;             /* bit 0: u mirrored, bit 1: v */
} paint_t;                      /* 48 bytes on the console */
#ifdef __XTENSA__
_Static_assert(sizeof(paint_t) == 48, "paint_t size");
#endif
/* x * m / 255 for x, m <= 255, exactly, without a divide */
static inline unsigned div255(unsigned t) { return (t + 1u + (t >> 8)) >> 8; }

static int paint_setup(const materials_t *m, const nm_material_binding_t *b, paint_t *p)
{
    memset(p, 0, sizeof *p);
    if (b->resource == NM_MATERIAL_SOLID) {
        p->pal[0] = rgb565(b->color);
        p->opaque = 0xffff;
        return 1;
    }
    const int palette = nm_material_palette(&m->pack, b);
    if (palette < 0 || b->resource >= m->pack.resources) return 0;
    const uint32_t mod = nm_material_argb(&m->pack, b);
    const unsigned mr = (mod >> 16) & 255, mg = (mod >> 8) & 255, mb = mod & 255;
    const uint32_t *pal = m->pack.palettes + 16u * (unsigned)palette;
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t c = pal[i];
        const unsigned r = div255(((c >> 16) & 255) * mr), g = div255(((c >> 8) & 255) * mg), bl = div255((c & 255) * mb);
        p->pal[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
        if ((c >> 24) >= 128) p->opaque |= (uint16_t)(1u << i);
    }
    /* only the punch-through class (header word 0 bits 13-14 = 3) tests texel alpha; opaque faces draw all */
    if (((b->header[0] >> 13) & 3u) != 3u) p->opaque = 0xffff;
    const nm_texture_record_t *rec = &m->pack.textures[b->resource];
    p->texture = m->texture + b->resource;
    p->wshift = (uint8_t)__builtin_ctz((unsigned)rec->width); p->hshift = (uint8_t)__builtin_ctz((unsigned)rec->height);
    if (b->color != UINT32_MAX) { p->u_origin = (uint16_t)(b->color & 65535); p->v_origin = (uint16_t)(b->color >> 16); }
    p->mirror = (uint8_t)(((b->header[0] & 0x100u) ? 1u : 0u) | ((b->header[0] & 0x200u) ? 2u : 0u));
    return 1;
}


/* ------------------------------------------------------------------ rasteriser */

typedef struct { float x, y, q, uq, vq; } svert_t;   /* screen x, y; 1/z; u/z; v/z (u, v in texels) */

/* Depth as 16 bits: the float bits of 1/z (times the depth class) above those of the sky's depth, >> 12. Positive
   floats order like their bits, so this keeps GEQUAL; 11 mantissa bits keep a relative step of 2^-11, finer
   than the decal layer step 2^-10. 1/z from 0.0001 (the sky) to 100 (the near plane) fits. */
#define SKY_BITS 0x38d1b717u
static inline uint16_t depth16(float q)
{
    union { float f; uint32_t u; } b = { q };
    if (b.u <= SKY_BITS) return 0;
    const uint32_t d = ((b.u - SKY_BITS) >> 12) + 1u;
    return d > 65535u ? 65535u : (uint16_t)d;
}

/* The same depth with 4 more fraction bits, as 16.16 fixed point, for stepping across spans and triangles:
   rounding only when a pixel is stored keeps a decal's 2-unit lead over its base face (NM_LAYER_DEPTH_STEP). */
static inline uint32_t depth_fx(float q)
{
    /* no range checks: faces beyond the sky are dropped and 1/z stays below 128 (the near plane), so the code
       stays inside 1..0xa2e5 (16.16) */
    union { float f; uint32_t u; } b = { q };
    return ((b.u - SKY_BITS) << 4) + 65536u;
}

/* Depth over a 16-pixel span as a quadratic in 16.16 fixed point, stepped with two integer adds per pixel:
   exact at the span's start, middle and end (1/z is linear on the screen, the depth code is close to its log).
   The cubic remainder stays far below a depth unit, so decals keep their lead; and no float work per pixel. */
static inline void depth_quad(float qs, float dqs, uint32_t *z, int32_t *dz, int32_t *ddz)
{
    const uint32_t d0 = depth_fx(qs), d1 = depth_fx(qs + 8.0f * dqs), d2 = depth_fx(qs + 16.0f * dqs);
    const int32_t a = (int32_t)(d1 - d0), b = (int32_t)(d2 - d1);
    const int32_t c2 = (b - a) / 64;              /* the second difference, 2c */
    *z = d0;
    *dz = a / 8 - (7 * c2) / 2;                   /* z(1) - z(0) = (d1 - d0)/8 - 7c */
    *ddz = c2;
}

#define SPAN 16  /* pixels between two perspective divides */

/* Reciprocal without a divide (the toolchain calls the ROM's software __divsf3 for every float division):
   a 256-entry table of 1/m for the mantissa's top 8 bits, the exponent negated by bit arithmetic, then one
   Newton step. Relative error about 2^-17, plenty for screen and texture coordinates. Positive and negative
   normal floats; not for 0, infinities or denormals (the renderer never passes them). */
static float recip_table[256];
static void recip_init(void);
static void twiddle_init(void);
static void tables_init(void)
{
    static int done;
    if (!done) { twiddle_init(); recip_init(); done = 1; }
}
static void recip_init(void)
{
    for (unsigned i = 0; i < 256; ++i) recip_table[i] = 1.0f / (1.0f + ((float)i + 0.5f) / 256.0f);
}
static inline float fast_recip(float x)
{
#ifdef __XTENSA__
    /* The FPU's own seed (recip0.s, about 7 bits: a table in silicon) and two Newton steps: 22 cycles and a
       relative error of 1.2e-7 measured on the console, against 26 cycles and 3.8e-6 for the table below. */
    float r;
    __asm__("recip0.s %0, %1" : "=f"(r) : "f"(x));
    r = r * (2.0f - x * r);
    return r * (2.0f - x * r);
#endif
    union { float f; uint32_t u; } in = { x }, t, out;
    const uint32_t e = (in.u >> 23) & 255u;
    t.f = recip_table[(in.u >> 15) & 255u];                   /* 1/m in (0.5, 1): exponent 126 */
    out.u = (in.u & 0x80000000u) | ((253u - e) << 23) | (t.u & 0x7fffffu);
    return out.f * (2.0f - x * out.f);                        /* one Newton step */
}

/* ceil for the screen range (|v| well below 2^31): no libm call */
static inline int fast_ceil(float v)
{
    const int i = (int)v;
    return (float)i < v ? i + 1 : i;
}


static inline int texel_coord(int32_t fixed, int mask, int shift, unsigned mirror)
{
    const int t = fixed >> 16;                     /* arithmetic shift: floor */
    int x = t & mask;
    if (mirror && ((t >> shift) & 1)) x = mask - x;
    return x;
}

/* The rows a core draws: even or odd (the two cores take one each: guide 6.2), or all of them (PIPE: one core
   draws the whole picture, each triangle set up once). RS: rows from one of them to the next. */
#if ALL_ROWS
#define RS 1
static inline int band_first(int y0, int core) { (void)core; return y0; }
#else
#define RS 2
static inline int band_first(int y0, int core) { return (y0 & 1) != core ? y0 + 1 : y0; }
#endif
/* One triangle, on one core's rows. */
/* A triangle ready to fill: sorted corners, edge slopes, attribute planes and its texture level, set up once
   by core 0 when the list is built; the two cores then only walk rows. */
typedef struct {
    float ax, ay, bx, by;            /* top and middle corner (screen) */
    float long_dx, top_dx, bot_dx;   /* x per row of a->c, a->b, b->c */
    float q0, uq0, vq0;              /* 1/z, u/z, v/z at the top corner */
    float dqdx, dqdy, dudx, dudy, dvdx, dvdy;
    float depth_scale;
    int16_t y0, y2;                  /* rows y0 .. y2-1 */
    uint16_t paint;
    uint8_t long_left, level, key;   /* key: the sort key, near first */
    uint8_t affine;                  /* bit 0: uq0/vq0 and their gradients hold u and v (texels) directly: no divide;
                                        bit 1: the depth code is a plane and the corners fit 16.16 (fixed-point rows) */
} tri_t;

static uint32_t cnt_rows[2], cnt_px[2], cnt_drawn[2];

#ifdef SW3D_OVERDRAW
/* measurement (host, one thread): rows and pixels visited that change nothing (hidden behind nearer ones) */
static unsigned long long od_px, od_written, od_rows, od_hidden_rows, od_hidden_row_px, od_tris, od_hidden_tris, od_hidden_tri_px;
static uint16_t od_snap[W], *od_pz;
static int od_n, od_tri_written;
static unsigned long long od_tri_cur;
static void od_flush(void)
{
    if (!od_pz) return;
    int w = 0;
    for (int i = 0; i < od_n; ++i) w += od_pz[i] != od_snap[i];
    od_px += (unsigned)od_n; od_written += (unsigned)w; ++od_rows; od_tri_cur += (unsigned)od_n;
    if (!w) { ++od_hidden_rows; od_hidden_row_px += (unsigned)od_n; } else od_tri_written = 1;
    od_pz = NULL;
}
static void od_row(uint16_t *pz, int n) { od_flush(); od_pz = pz; od_n = n; memcpy(od_snap, pz, sizeof(uint16_t) * (size_t)n); }
#define OD_ROW(pz, n) od_row(pz, n)
#else
#define OD_ROW(pz, n) ((void)0)
#endif
#ifdef SW3D_PROFILE
static uint32_t cyc_tri[2], cyc_row[2], cyc_span[2], cyc_kernel[2], cyc_pro[2], cnt_spans[2], cnt_iter[2];
static uint32_t cyc_build[2], cyc_face[2], cyc_setup[2], cyc_cars[2], cnt_face_iter[2], cnt_setup[2];
static uint32_t cyc_fix_pro[2], cyc_fix_touch[2], cyc_fix_k[2];
static uint32_t cyc_fix[2], cnt_fix_tri[2], cnt_fix_iter[2], cnt_fix_px[2], cnt_tri[2], cnt_px_all[2];
static uint32_t cyc_mbox[2], cnt_inst[2], cnt_inst_vis[2], cyc_grp[2], cnt_grp[2], cyc_lod[2], cyc_rear[2], cyc_bind[2], cnt_find[2], cyc_paint[2], cnt_pset[2];
static uint32_t cyc_xf[2], cyc_mip[2];
#define PROF(var, core, stmt) do { const uint32_t p0_ = CYCLES(); stmt; var[core] += CYCLES() - p0_; } while (0)
#define PROF_SPAN(core) (++cnt_spans[core])
#else
#define PROF(var, core, stmt) do { stmt; } while (0)
#define PROF_SPAN(core) ((void)0)
#endif

/* The textured span kernel: n pixels, u v z stepping in fixed point (z is the depth16 value in 16.16, stepped
   linearly across the span: depth16 is close to linear over 16 pixels). Mirror and punch-through are
   compile-time constants of each instance, so the loop keeps its few variables in registers. */
/* quad: depth is a quadratic (dz steps by ddz), else a line. vs: v comes pre-multiplied by the level's width
   (v << wshift, wrapping) and hmask likewise, so the texel index needs no variable shift (not with mirror v). */
static inline __attribute__((always_inline)) void span_kernel(uint16_t *row, uint16_t *zrow, int n, uint32_t u, uint32_t v,
        uint32_t du, uint32_t dv, uint32_t z, int32_t dz, int32_t ddz, const uint8_t *texels, int wshift, int wmask, int hmask,
        const uint16_t *pal, uint16_t opaque, const int mu, const int mv, const int pt, const int quad, const int vs)
{
    for (; n; --n, ++row, ++zrow, u += du, v += dv, z += (uint32_t)dz, dz += quad ? ddz : 0) {
        const uint16_t zz = (uint16_t)(z >> 16);
        if (zz < *zrow) continue;
        uint32_t idx;
        if (vs) {
            uint32_t tx = (u >> 16) & (uint32_t)wmask;
            if (mu && ((u >> 16) >> wshift & 1)) tx = (uint32_t)wmask - tx;
            idx = ((v >> 16) & (uint32_t)hmask) | tx;
        } else {
            int tx = (int)(u >> 16) & wmask, ty = (int)(v >> 16) & hmask;
            if (mu && ((u >> 16) >> wshift & 1)) tx = wmask - tx;
            if (mv && ((v >> 16) & (uint32_t)(hmask + 1))) ty = hmask - ty;
            idx = (uint32_t)(ty << wshift) | (uint32_t)tx;
        }
        const uint8_t byte = texels[idx >> 1];
        const unsigned texel = idx & 1 ? byte >> 4 : byte & 15u;
        if (pt && !(opaque >> texel & 1u)) continue;
        *zrow = zz;
        *row = pal[texel];
    }
}

static void span_textured(uint16_t *row, uint16_t *zrow, int n, int32_t u, int32_t v, int32_t du, int32_t dv,
        uint32_t z, int32_t dz, int32_t ddz, const uint8_t *texels, int wshift, int wmask, int hmask, const uint16_t *pal,
        uint16_t opaque, unsigned variant)
{
    switch (variant) {
#define K(i) case i: span_kernel(row, zrow, n, u, v, du, dv, z, dz, ddz, texels, wshift, wmask, hmask, pal, opaque, i & 1, (i >> 1) & 1, (i >> 2) & 1, 1, 0); break;
    K(0) K(1) K(2) K(3) K(4) K(5) K(6) K(7)
#undef K
    }
}

static const float inv_len[SPAN + 1] = { 0, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8,
    1.0f / 9, 1.0f / 10, 1.0f / 11, 1.0f / 12, 1.0f / 13, 1.0f / 14, 1.0f / 15, 1.0f / 16 };

/* Depth over a span starting at d0 = depth_fx(qs): the step and second difference for the span kernel, and the
   value at its end (the next span's start). A full span: the quadratic through its start, middle and end (the
   code is close to log 1/z; 1/z is linear). Up to 4 pixels: a straight line (curvature well under a unit).
   Longer partial spans: the quadratic through start, middle and end of their own length. */
static inline __attribute__((always_inline)) void span_depth(uint32_t d0, float qs, float qs_dx, int len,
                                                             int32_t *dz, int32_t *ddz, uint32_t *dend)
{
    /* the quadratic through the code at 0, 8 and 16 pixels; a shorter span uses its first len pixels */
    const uint32_t d1 = depth_fx(qs + 8.0f * qs_dx), d2 = depth_fx(qs + 16.0f * qs_dx);
    const int32_t a = (int32_t)(d1 - d0), c2 = ((int32_t)(d2 - d1) - a) >> 6;
    *dz = (a >> 3) - ((7 * c2) >> 1); *ddz = c2;
    (void)len;
    *dend = d2;   /* the next span's start (only full spans have a next one) */
}

/* float to 16.16 */
#define FX(f) ((int32_t)((f) * 65536.0f))

/* n pixels of a perspective row from r/zr, depth worked out from 1/z: qsd is 1/z times the depth scale at the
   first pixel, qxs its step. Within a binade (one float exponent) the depth code is linear in 1/z, so a run is
   one kernel call with a linear depth; a run that crosses a power of two is cut there (the exponent changes
   the slope). u, v (v pre-multiplied by the width when !mv) step by du, dv. tex = 0: a solid colour. */
static inline __attribute__((always_inline)) void depth_runs(uint16_t *r, uint16_t *zr, int n, float qsd, float qxs,
        uint32_t u, uint32_t v, uint32_t du, uint32_t dv, const uint8_t *texels, int wshift, int wmask, int hm,
        const uint16_t *pal, uint16_t opaque, uint16_t colour, const int mu, const int mv, const int pt, const int tex,
        int parity)
{
    (void)parity;
    for (;;) {
        const union { float f; uint32_t u; } b = { qsd };
        const uint32_t e = b.u >> 23;
        const union { uint32_t u; float f; } zsc = { (281u - e) << 23 };   /* 2^(154 - e): code units per unit */
        const int32_t zx = (int32_t)(qxs * zsc.f);
        const uint32_t z0 = ((b.u - SKY_BITS) << 4) + 65536u;          /* depth_fx */
        int m = n;
        const union { float f; uint32_t u; } be = { qsd + qxs * (float)(n - 1) };
        if (__builtin_expect((be.u >> 23) != e, 0)) {   /* the run's last pixel is in another binade */
            const union { uint32_t u; float f; } bound = { (qxs > 0.0f ? e + 1u : e) << 23 };
            int n1 = fast_ceil((bound.f - qsd) * fast_recip(qxs));
            if (n1 < 1) n1 = 1;
            if (n1 < m) m = n1;
        }
        if (tex)
            PROF(cyc_fix_k, parity, span_kernel(r, zr, m, u, v, du, dv, z0, zx, 0, texels, wshift, wmask, hm, pal, opaque,
                                                mu, mv, pt, 0, !mv));
        else {
            uint32_t zc = z0;
            uint16_t *rr = r, *zz = zr;
            for (int k = m; k; --k, ++rr, ++zz, zc += (uint32_t)zx) {
                const uint16_t dd = (uint16_t)(zc >> 16);
                if (dd >= *zz) { *zz = dd; *rr = colour; }
            }
        }
        n -= m;
        if (!n) return;
        r += m; zr += m; u += (uint32_t)m * du; v += (uint32_t)m * dv; qsd += qxs * (float)m;
    }
}

/* A triangle with fixed-point rows (tri_t.affine bit 1): the edges step in 16.16 and everything per row is
   integer but the perspective divide. The FPU's results take 4 cycles to use (and a compare and branch on one 4
   more), the integer unit's 1, so the float scanline of thousands of 7-pixel rows was latency, not work.
   persp = 0 (affine or solid triangles, whose depth code is a plane: one binade): depth, u and v are kept at the
   row's left pixel and step by a DDA (per row their y step plus the left pixel's whole-pixel move times their x
   step). Values are only taken at pixels on the triangle's left edge, so nothing is extrapolated far.
   persp = 1: 1/z, u/z and v/z step along the left edge the same way (as floats); u and v come from a divide
   every SPAN pixels and the depth from 1/z per run (depth_runs: any number of binades). */
static inline __attribute__((always_inline)) void raster_rows_fixed(const tri_t *t, const paint_t *p, int parity,
                                                                    const int mu, const int mv, const int pt, const int persp)
{
#ifdef SW3D_PROFILE
    const uint32_t fp0_ = CYCLES();
#endif
    int y = band_first(t->y0, parity);
    const int y2 = t->y2;
    if (y >= y2) return;
#ifdef SW3D_PROFILE
    { volatile const void *pt_ = p->texture; volatile uint16_t pp_ = p->pal[15]; volatile float ff_ = t->dvdy; (void)pt_; (void)pp_; (void)ff_;
      cyc_fix_touch[parity] += CYCLES() - fp0_; }
#endif
    const uint8_t *texels = NULL;
    int wshift = 0, hshift = 0, wmask = 0, hmask = 0;
    float fixed_scale = 65536.0f;
    if (p->texture) {
        unsigned level = t->level;
        if (level >= p->texture->levels) level = p->texture->levels - 1u;
        texels = p->texture->data + p->texture->offset[level];
        wshift = p->wshift - (int)level;
        hshift = p->hshift - (int)level;
        if (wshift < 0) wshift = 0;
        if (hshift < 0) hshift = 0;
        wmask = (1 << wshift) - 1; hmask = (1 << hshift) - 1;
        fixed_scale = (float)(65536u >> level);
#ifdef SW3D_FAKE_TEX
        static uint8_t fake_fx[64 * 64 / 2];   /* measurement: one texture in internal RAM for every face */
        texels = fake_fx; wshift = hshift = 6; wmask = hmask = 63;
#endif
    }
    const float ax = t->ax, ay = t->ay, py = (float)y + 0.5f;
    /* edges: x in 16.16; a row's first pixel is ceil(x - 0.5) = (x + 0x7fff) >> 16 */
    int32_t xlong = FX(ax + (py - ay) * t->long_dx);
    const int32_t xlong_step = FX((float)RS * t->long_dx);
    const int ymid = fast_ceil(t->by - 0.5f);   /* the first row on the lower short edge */
    const int long_left = t->long_left;
    int32_t xtop = 0, xtop_step = 0;
    if (y < ymid) { xtop = FX(ax + (py - ay) * t->top_dx); xtop_step = FX((float)RS * t->top_dx); }
    int xl = (int)(((long_left ? xlong : y < ymid ? xtop : FX(t->bx + (py - t->by) * t->bot_dx)) + 0x7fff) >> 16);
    const float ds = t->depth_scale;
    const float ox = (float)xl + 0.5f - ax, oy = py - ay;
    /* persp = 0: depth exact at corner a (its bits), a plane from there in code units */
    uint32_t z = 0;
    int32_t zx = 0, zy = 0;
    if (!persp) {
        union { float f; uint32_t u; } qa = { t->q0 * ds };
        const union { uint32_t u; float f; } zs = { (281u - (qa.u >> 23)) << 23 };   /* 2^(154 - e) */
        const float zk = ds * zs.f;
        z = depth_fx(qa.f) + (uint32_t)(int32_t)((ox * t->dqdx + oy * t->dqdy) * zk);
        zx = (int32_t)(t->dqdx * zk); zy = (int32_t)((float)RS * t->dqdy * zk);
        z -= (uint32_t)zy;   /* one row step back */
    }
    /* u, v in the level's 16.16 texels, less whole mirror periods (two widths: a period of every case) */
    uint32_t u = 0, v = 0;
    int32_t ux = 0, uy = 0, vx = 0, vy = 0, hm = hmask;
    float ql = 0, uql = 0, vql = 0, qy = 0, uqy = 0, vqy = 0, uo = 0, vo = 0;
    const float qx = t->dqdx, uqx = t->dudx, vqx = t->dvdx, fs = fixed_scale, qxs = qx * ds;
    if (persp) {   /* 1/z (u/z, v/z) at the first row's left pixel, and per two rows; one row step back */
        qy = (float)RS * t->dqdy;
        ql = t->q0 + ox * qx + oy * t->dqdy - qy;
        if (texels) {
            uqy = (float)RS * t->dudy; vqy = (float)RS * t->dvdy;
            uql = t->uq0 + ox * uqx + oy * t->dudy - uqy;
            vql = t->vq0 + ox * vqx + oy * t->dvdy - vqy;
            uo = (float)p->u_origin * fs; vo = (float)p->v_origin * fs;
            if (!mv) hm = hmask << wshift;
        }
    }
    if (texels && !persp) {
        const float uf = (t->uq0 + ox * t->dudx + oy * t->dudy - (float)p->u_origin) * fs;
        const float vf = (t->vq0 + ox * t->dvdx + oy * t->dvdy - (float)p->v_origin) * fs;
        const float pu = (float)(2u << wshift) * 65536.0f, pv = (float)(2u << hshift) * 65536.0f;
        const float ku = uf * fast_recip(pu), kv = vf * fast_recip(pv);
        int iu = (int)ku, iv = (int)kv;
        if ((float)iu > ku) --iu;
        if ((float)iv > kv) --iv;
        u = (uint32_t)(int32_t)(uf - (float)iu * pu);
        v = (uint32_t)(int32_t)(vf - (float)iv * pv);
        ux = (int32_t)(t->dudx * fs); uy = (int32_t)((float)RS * t->dudy * fs);   /* fs is already the 16.16 scale */
        vx = (int32_t)(t->dvdx * fs); vy = (int32_t)((float)RS * t->dvdy * fs);
        if (!mv) {   /* v pre-multiplied by the width: no shift per pixel */
            v <<= wshift; vx = (int32_t)((uint32_t)vx << wshift); vy = (int32_t)((uint32_t)vy << wshift);
            hm = hmask << wshift;
        }
        u -= (uint32_t)uy; v -= (uint32_t)vy;   /* one row step back */
    }
    const uint16_t opaque = p->opaque, colour = p->pal[0];
    const uint16_t *pal = p->pal;
    uint16_t *const pix = G.pixels, *const dep = G.depth;
    int off = y * W;
#ifdef SW3D_PROFILE
    cyc_fix_pro[parity] += CYCLES() - fp0_;
#endif
    /* Two runs of rows: above the middle corner (the top short edge) and below it (the bottom one), each with
       a plain left and right edge. Every row starts with the step from the row before (the values were set one
       step back), so the first row needs no case of its own. */
    for (int seg = 0; seg < 2; ++seg) {
        int yend;
        int32_t xs, xs_step;
        if (seg == 0) { yend = ymid < y2 ? ymid : y2; xs = xtop; xs_step = xtop_step; }
        else { yend = y2; xs = FX(t->bx + ((float)y + 0.5f - t->by) * t->bot_dx); xs_step = FX((float)RS * t->bot_dx); }
        if (y >= yend) continue;
        int32_t xa = long_left ? xlong : xs, xb = long_left ? xs : xlong;
        const int32_t xas = long_left ? xlong_step : xs_step, xbs = long_left ? xs_step : xlong_step;
        const int rows = (yend - y + RS - 1) / RS;
        for (int r = rows; r; --r, xa += xas, xb += xbs, off += RS * W) {
#ifdef SW3D_PROFILE
            ++cnt_fix_iter[parity];
#endif
            const int nxl = (int)((xa + 0x7fff) >> 16), xr = (int)((xb + 0x7fff) >> 16);
            const int d = nxl - xl;
            xl = nxl;
            if (persp) {
                const float fd = (float)d;
                ql += qy + fd * qx;
                if (texels) { uql += uqy + fd * uqx; vql += vqy + fd * vqx; }
            } else {
                z += (uint32_t)(zy + d * zx);
                u += (uint32_t)(uy + d * ux);
                v += (uint32_t)(vy + d * vx);
            }
            const int x0 = xl > 0 ? xl : 0, x1 = xr < W ? xr : W;
            if (x0 >= x1) continue;
#ifdef SW3D_PROFILE
            ++cnt_rows[parity]; cnt_px[parity] += (uint32_t)(x1 - x0);
#endif
            const int pre = x0 - xl;   /* pixels left of the view */
#ifdef SW3D_DUMMY_FB
            static uint16_t dummy_rows[2][W];   /* measurement: pixels written to an internal-RAM row, not the PSRAM picture */
            uint16_t *const prow = dummy_rows[parity];
#else
            uint16_t *const prow = pix + off;
#endif
            uint16_t *const pz = dep + off;
            OD_ROW(pz + x0, x1 - x0);
            if (persp) {
                const float fpre = (float)pre;
                float q = ql + fpre * qx;
                if (!texels) {   /* a solid colour: the row's depth in runs */
                    depth_runs(prow + x0, pz + x0, x1 - x0, q * ds, qxs, 0, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, colour,
                               mu, mv, pt, 0, parity);
                    continue;
                }
                /* a divide every SPAN pixels, u and v stepped linearly between */
                float uq = uql + fpre * uqx, vq = vql + fpre * vqx;
                float rz = fast_recip(q);
                int32_t u0 = (int32_t)(uq * rz * fs - uo), v0 = (int32_t)(vq * rz * fs - vo);
                uint16_t *r = prow + x0, *zr = pz + x0;
                for (int n = x1 - x0; n > 0;) {
                    const int len = n < SPAN ? n : SPAN;
                    const float fl = (float)len, qs = q * ds;
                    q += qx * fl; uq += uqx * fl; vq += vqx * fl;
                    rz = fast_recip(q);
                    const int32_t u1 = (int32_t)(uq * rz * fs - uo), v1 = (int32_t)(vq * rz * fs - vo);
                    int32_t du, dv;
                    if (len == SPAN) { du = (u1 - u0) >> 4; dv = (v1 - v0) >> 4; }
                    else { du = (int32_t)((float)(u1 - u0) * inv_len[len]); dv = (int32_t)((float)(v1 - v0) * inv_len[len]); }
                    const uint32_t vs0 = mv ? (uint32_t)v0 : (uint32_t)v0 << wshift, dvs = mv ? (uint32_t)dv : (uint32_t)dv << wshift;
                    depth_runs(r, zr, len, qs, qxs, (uint32_t)u0, vs0, (uint32_t)du, dvs, texels, wshift, wmask, hm, pal, opaque,
                               colour, mu, mv, pt, 1, parity);
                    r += len; zr += len; n -= len;
                    u0 = u1; v0 = v1;
                }
                continue;
            }
            const uint32_t zz = z + (uint32_t)(pre * zx);
            if (texels)
                PROF(cyc_fix_k, parity, span_kernel(prow + x0, pz + x0, x1 - x0, u + (uint32_t)(pre * ux), v + (uint32_t)(pre * vx), (uint32_t)ux,
                            (uint32_t)vx, zz, zx, 0, texels, wshift, wmask, hm, pal, opaque, mu, mv, pt, 0, !mv));
            else {
                uint16_t *r = prow + x0, *zr = pz + x0;
                uint32_t zc = zz;
                for (int k = x1 - x0; k; --k, ++r, ++zr, zc += (uint32_t)zx) {
                    const uint16_t dd = (uint16_t)(zc >> 16);
                    if (dd >= *zr) { *zr = dd; *r = colour; }
                }
            }
        }
        y += RS * rows;
        xlong = long_left ? xa : xb;
    }
}

/* The rows of one triangle on one core's parity. Instantiated once per texture variant (mirror u, mirror v,
   punch-through as compile-time constants), so that the span kernel is inlined: no call per span. */
static inline __attribute__((always_inline)) void raster_rows(const tri_t *t, const paint_t *p, int parity,
                                                              const int mu, const int mv, const int pt)
{
#ifndef SW3D_NO_FIXED
    if (t->affine & 2) {
        const int persp = !(t->affine & 1);   /* affine triangles (and solid ones flagged so) have a depth plane */
#ifdef SW3D_PROFILE
        const uint32_t f0_ = CYCLES(); const uint32_t px0_ = cnt_px[parity];
#endif
        if (persp) raster_rows_fixed(t, p, parity, mu, mv, pt, 1);
        else raster_rows_fixed(t, p, parity, mu, mv, pt, 0);
#ifdef SW3D_PROFILE
        cyc_fix[parity] += CYCLES() - f0_; ++cnt_fix_tri[parity]; cnt_fix_px[parity] += cnt_px[parity] - px0_;
#endif
        return;
    }
#endif
#ifdef SW3D_PROFILE
    const uint32_t pro0_ = CYCLES();
#endif
    int y0 = band_first(t->y0, parity);
    const int y2 = t->y2;
    if (y0 >= y2) return;
    const float ax = t->ax, ay = t->ay, bx = t->bx, by = t->by;
    const float dqdx = t->dqdx, dqdy = t->dqdy, dudx = t->dudx, dudy = t->dudy, dvdx = t->dvdx, dvdy = t->dvdy;
    const float depth_scale = t->depth_scale, qs_dx = dqdx * depth_scale;
    /* the texture level of this triangle: sizes and the fixed-point scale halve per level */
    const uint8_t *texels = NULL;
    int wshift = 0, hshift = 0, wmask = 0, hmask = 0;
    float fixed_scale = 65536.0f;
    if (p->texture) {
        unsigned level = t->level;
        if (level >= p->texture->levels) level = p->texture->levels - 1u;
        texels = p->texture->data + p->texture->offset[level];
        wshift = p->wshift - (int)level; hshift = p->hshift - (int)level;
        if (wshift < 0) wshift = 0;
        if (hshift < 0) hshift = 0;
        wmask = (1 << wshift) - 1; hmask = (1 << hshift) - 1;
        fixed_scale = (float)(65536u >> level);
#ifdef SW3D_FAKE_TEX
        /* measurement: every face samples one 128x128 texture in internal RAM (no texture cache misses) */
        static uint8_t fake[64 * 64 / 2];
        texels = fake; wshift = hshift = 6; wmask = hmask = 63;
#endif
    }
    const float uo = (float)p->u_origin * fixed_scale, vo = (float)p->v_origin * fixed_scale;
    const uint16_t opaque = p->opaque;
    /* Incremental rows: the edges and each attribute's value at x = 0 of the row step by a constant per row
       (two rows: the other core takes the others); a row then needs one multiply-add per attribute at x0. */
    const float py0 = (float)y0 + 0.5f;
    float xlong = ax + (py0 - ay) * t->long_dx;
    const float xlong_step = (float)RS * t->long_dx, top_step = (float)RS * t->top_dx, bot_step = (float)RS * t->bot_dx;
    int on_top = py0 < by;
    float xs = on_top ? ax + (py0 - ay) * t->top_dx : bx + (py0 - by) * t->bot_dx;
    /* planes at pixel centres: A(x, y) = A0 + (x + 0.5 - ax) dA/dx + (y + 0.5 - ay) dA/dy */
    float qsrow = (t->q0 + (0.5f - ax) * dqdx + (py0 - ay) * dqdy) * depth_scale;
    const float qsrow_step = (float)RS * dqdy * depth_scale;
    float qrow = t->q0 + (0.5f - ax) * dqdx + (py0 - ay) * dqdy, uqrow = 0, vqrow = 0;
    const float qrow_step = (float)RS * dqdy;
    float urow = 0, vrow = 0, urow_step = 0, vrow_step = 0, ux = 0, vx = 0;
    if (texels) {
        uqrow = t->uq0 + (0.5f - ax) * dudx + (py0 - ay) * dudy;
        vqrow = t->vq0 + (0.5f - ax) * dvdx + (py0 - ay) * dvdy;
        if (t->affine & 1) {   /* u, v themselves, in fixed point units */
            urow = uqrow * fixed_scale - uo; vrow = vqrow * fixed_scale - vo;
            urow_step = (float)RS * dudy * fixed_scale; vrow_step = (float)RS * dvdy * fixed_scale;
            ux = dudx * fixed_scale; vx = dvdx * fixed_scale;
        }
    }
    const float uqrow_step = (float)RS * dudy, vqrow_step = (float)RS * dvdy;
#ifdef SW3D_PROFILE
    cyc_pro[parity] += CYCLES() - pro0_;
#endif
    for (int y = y0; y < y2; y += RS, xlong += xlong_step, xs += on_top ? top_step : bot_step, qsrow += qsrow_step,
         qrow += qrow_step, uqrow += uqrow_step, vqrow += vqrow_step, urow += urow_step, vrow += vrow_step) {
        const float py = (float)y + 0.5f;
#ifdef SW3D_PROFILE
        ++cnt_iter[parity];
#endif
        if (on_top && py >= by) { on_top = 0; xs = bx + (py - by) * t->bot_dx; }
        const float xl = t->long_left ? xlong : xs, xr = t->long_left ? xs : xlong;
        int x0 = fast_ceil(xl - 0.5f), x1 = fast_ceil(xr - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > W) x1 = W;
        if (x0 >= x1) continue;
#ifdef SW3D_PROFILE
        ++cnt_rows[parity]; cnt_px[parity] += (uint32_t)(x1 - x0);
#endif
#ifdef SW3D_NO_PIXELS
        continue;   /* measurement: the triangle and row overhead alone */
#endif
        const float fx0 = (float)x0;
        uint16_t *row = G.pixels + y * W + x0;
        uint16_t *zrow = G.depth + y * W + x0;
        int n = x1 - x0;
        OD_ROW(zrow, n);
        float qs = qsrow + fx0 * qs_dx;
        uint32_t d0 = depth_fx(qs);
        if (!texels) {   /* a solid colour: depth only */
            const uint16_t colour = p->pal[0];
            while (n > 0) {
                const int len = n < SPAN ? n : SPAN;
                int32_t dz, ddz; uint32_t dend;
                span_depth(d0, qs, qs_dx, len, &dz, &ddz, &dend);
                uint32_t z = d0;
                for (int k = len; k; --k, ++row, ++zrow, z += (uint32_t)dz, dz += ddz) {
                    const uint16_t d = (uint16_t)(z >> 16);
                    if (d >= *zrow) { *zrow = d; *row = colour; }
                }
                qs += qs_dx * (float)len; n -= len; d0 = dend;
            }
            continue;
        }
        if (t->affine & 1) {   /* u, v straight from their planes */
            int32_t u = (int32_t)(urow + fx0 * ux), v = (int32_t)(vrow + fx0 * vx);
            const int32_t du = (int32_t)ux, dv = (int32_t)vx;
            while (n > 0) {
                const int len = n < SPAN ? n : SPAN;
                int32_t dz, ddz; uint32_t dend;
                span_depth(d0, qs, qs_dx, len, &dz, &ddz, &dend);
                PROF_SPAN(parity); PROF(cyc_kernel, parity, span_kernel(row, zrow, len, u, v, du, dv, d0, dz, ddz, texels, wshift, wmask, hmask,
                            p->pal, opaque, mu, mv, pt, 1, 0));
                row += len; zrow += len; qs += qs_dx * (float)len;
                u += du * len; v += dv * len; n -= len; d0 = dend;
            }
            continue;
        }
        float q = qrow + fx0 * dqdx;
        float uq = uqrow + fx0 * dudx;
        float vq = vqrow + fx0 * dvdx;
        float rz = fast_recip(q);
        int32_t u0 = (int32_t)(uq * rz * fixed_scale - uo), v0 = (int32_t)(vq * rz * fixed_scale - vo);
        while (n > 0) {
            const int len = n < SPAN ? n : SPAN;
            q += dqdx * len; uq += dudx * len; vq += dvdx * len;
            rz = fast_recip(q);
            const int32_t u1 = (int32_t)(uq * rz * fixed_scale - uo), v1 = (int32_t)(vq * rz * fixed_scale - vo);
            int32_t du, dv;
            if (len == SPAN) { du = (u1 - u0) >> 4; dv = (v1 - v0) >> 4; }
            else { du = (int32_t)((float)(u1 - u0) * inv_len[len]); dv = (int32_t)((float)(v1 - v0) * inv_len[len]); }
            int32_t dz, ddz; uint32_t dend;
            span_depth(d0, qs, qs_dx, len, &dz, &ddz, &dend);
            PROF_SPAN(parity); PROF(cyc_kernel, parity, span_kernel(row, zrow, len, u0, v0, du, dv, d0, dz, ddz, texels, wshift, wmask, hmask,
                        p->pal, opaque, mu, mv, pt, 1, 0));
            row += len; zrow += len; qs += qs_dx * (float)len;
            u0 = u1; v0 = v1; n -= len; d0 = dend;
        }
    }
}

#if defined(SW3D_VARIANTS) && defined(SW3D_PROFILE)
unsigned sw3d_var_px[16];
#endif
/* Mirrored textures (none in the race scenes measured): in flash, so that the instruction RAM they would take
   (it comes out of the same internal SRAM as the heap) stays free for the depth buffer. */
static __attribute__((noinline)) void raster_body_mirror(const tri_t *t, const paint_t *p, int parity, unsigned variant)
{
    switch (variant) {
#define R(i) case i: raster_rows(t, p, parity, i & 1, (i >> 1) & 1, (i >> 2) & 1); break;
    R(1) R(2) R(3) R(5) R(6) R(7)
#undef R
    }
}
static void raster_body(const tri_t *t, const paint_t *p, int parity)
{
#ifdef SW3D_PROFILE
    const uint32_t r0_ = CYCLES();
#endif
    const unsigned variant = (p->mirror & 3u) | (p->opaque != 0xffff ? 4u : 0u);
#ifdef SW3D_PROFILE
    ++cnt_tri[parity];
#endif
#if defined(SW3D_VARIANTS) && defined(SW3D_PROFILE)
    extern unsigned sw3d_var_px[16]; const uint32_t vpx0_ = cnt_px[parity];
#endif
#ifdef SW3D_OVERDRAW
    od_flush(); od_tri_written = 0; od_tri_cur = 0;
#endif
    if (variant & 3) raster_body_mirror(t, p, parity, variant);
    else if (variant & 4) raster_rows(t, p, parity, 0, 0, 1);
    else raster_rows(t, p, parity, 0, 0, 0);
#ifdef SW3D_OVERDRAW
    od_flush(); ++od_tris;
    if (!od_tri_written && od_tri_cur) { ++od_hidden_tris; od_hidden_tri_px += od_tri_cur; }
#endif
#if defined(SW3D_VARIANTS) && defined(SW3D_PROFILE)
    sw3d_var_px[variant + (p->texture ? 0 : 8) + 0] += cnt_px[parity] - vpx0_;
#endif
#ifdef SW3D_PROFILE
    cyc_tri[parity] += CYCLES() - r0_;
#endif
}

/* ------------------------------------------------------------------ the frame's triangle list */

/* Each core transforms, lights and clips half of the faces (alternate groups of 8) into its own list; then
   both cores fill both lists, each its rows. */
/* Triangles a frame can hold: PIPE, one array per set that the two lists share (core 0 builds them one after
   the other; the largest frame measured had 3,328); otherwise one array per list (both cores build at once). An
   order entry is list << 15 | index. */
#if PIPE
#define TRI_SET_CAP 5000u   /* 3,400 dropped the cars of the race's heavy frames and parts of Expert's world */
#else
#define TRI_SET_CAP 5400u
#define TRI_CAP 2700u
#endif
#define SETS (PIPE ? 2 : 1)
/* The frame's paints, one buffer for both cores: a binding gets one paint a frame, whichever core meets it
   first (each core used to make its own, about half of them twice). A heavy frame needs 330 to 750. The first
   g_paint_fast are in internal RAM (every triangle reads its paint on both cores), the rest in PSRAM. */
#define PAINT_CAP (PIPE ? 1536u : 2048u)   /* per set; a race frame needs up to about 1,420 (896 dropped the cars) */
#ifndef PAINT_FAST_MAX
#define PAINT_FAST_MAX 1024u
#endif
#define PAINT_HASH 2048u          /* binding -> paint: open addressing, a power of two */
typedef struct { paint_t *fast, *more; unsigned nfast; } pool_t;
static pool_t g_pool[SETS];                 /* per set (PIPE: the one being built and the one being drawn) */
static unsigned g_build_set;                /* the set the list is built into */
static uint32_t g_paint_count;
static uint32_t g_paint_hash[PAINT_HASH];   /* (key + 1) << 12 | index; index 0xfff: being set up, 0xffe: none */
static inline paint_t *paint_in(const pool_t *pl, unsigned i)
{
    return i < pl->nfast ? pl->fast + i : pl->more + (i - pl->nfast);
}
static inline paint_t *paint_at(unsigned i) { return paint_in(g_pool + g_build_set, i); }
static tri_t *g_tris[SETS][2];              /* [set][list] */
typedef struct {
    tri_t *tris;
    unsigned tri_count, faces;
    const nm_material_binding_t *last_binding;
    int last_paint;
    uint8_t *group_seen;   /* per world face group: 0 not tested for this instance, 1 visible, 2 culled */
    uint8_t *lod_level;    /* per level-of-detail group: the level picked for this instance (0xff: not yet) */
    uint8_t *keys;         /* the sort key of each triangle, in internal RAM (the list is in PSRAM): in g_keys */
    unsigned cap;          /* triangles this list may hold */
} list_t;
static list_t g_list[2];

/* Sets up one triangle for the fill; 0 when it covers no pixel centre of the view. */
static int tri_setup(tri_t *t, const svert_t *a, const svert_t *b, const svert_t *c, uint16_t paint, unsigned level,
                     float depth_scale)
{
    if (b->y < a->y) { const svert_t *x = a; a = b; b = x; }
    if (c->y < a->y) { const svert_t *x = a; a = c; c = x; }
    if (c->y < b->y) { const svert_t *x = b; b = c; c = x; }
    if (fmaxf(a->x, fmaxf(b->x, c->x)) < 0.0f || fminf(a->x, fminf(b->x, c->x)) > (float)W) return 0;
    int y0 = fast_ceil(a->y - 0.5f), y2 = fast_ceil(c->y - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y2 > H) y2 = H;
    if (y0 >= y2) return 0;
    const float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (fabsf(area) < 1e-6f) return 0;
    const float ia = fast_recip(area);
#define GRAD(F, dx, dy) t->dx = (((b->F - a->F) * (c->y - a->y) - (c->F - a->F) * (b->y - a->y)) * ia); \
                        t->dy = (((c->F - a->F) * (b->x - a->x) - (b->F - a->F) * (c->x - a->x)) * ia)
    GRAD(q, dqdx, dqdy);
    GRAD(uq, dudx, dudy);
    GRAD(vq, dvdx, dvdy);
#undef GRAD
    /* Affine when the error it makes stays near a pixel: the 1/z spread over the triangle times its size on the
       screen (far and small triangles): u, v and depth16 are then planes on the screen, with no divide per row or
       span. Near, large faces (a car alongside) keep the perspective path. */
    const float qmin = fminf(a->q, fminf(b->q, c->q)), qmax = fmaxf(a->q, fmaxf(b->q, c->q));
    const float extent = fmaxf(c->y - a->y, fmaxf(a->x, fmaxf(b->x, c->x)) - fminf(a->x, fminf(b->x, c->x)));
    /* The depth code is the float bits of 1/z, which are linear in 1/z within one binade (one exponent), and 1/z
       is linear on the screen; so when the three corners share the exponent the code is a plane and steps with
       integer adds (an affine triangle's rows). Otherwise it is worked out per span from 1/z (perspective rows). */
    int plane = 0;
    {
        union { float f; uint32_t u; } ea = { a->q * depth_scale }, eb = { b->q * depth_scale }, ec = { c->q * depth_scale };
        const uint32_t e = ea.u >> 23;
        if ((eb.u >> 23) == e && (ec.u >> 23) == e) {
            union { float f; uint32_t u; } zs = { .u = (281u - e) << 23 };   /* 2^(154 - e): code units per unit of 1/z */
            const float k = depth_scale * zs.f;
            const float lim = 67108864.0f;   /* 2^26 */
            plane = fabsf(t->dqdx * k) < lim && fabsf(t->dqdy * k) < lim;
        }
    }
#ifdef SW3D_NO_AFFINE
    t->affine = 0; (void)extent;
#else
    t->affine = plane && (qmax - qmin) * extent <= 1.5f * qmin;
#endif
    if (t->affine) {
        const float ra = fast_recip(a->q), rb = fast_recip(b->q), rc = fast_recip(c->q);
        const float ua = a->uq * ra, ub = b->uq * rb, uc = c->uq * rc;
        const float va = a->vq * ra, vb = b->vq * rb, vc = c->vq * rc;
#define PLANE(A, B, C, dx, dy) t->dx = (((B - A) * (c->y - a->y) - (C - A) * (b->y - a->y)) * ia); \
                               t->dy = (((C - A) * (b->x - a->x) - (B - A) * (c->x - a->x)) * ia)
        PLANE(ua, ub, uc, dudx, dudy);
        PLANE(va, vb, vc, dvdx, dvdy);
#undef PLANE
    }
    t->ax = a->x; t->ay = a->y; t->bx = b->x; t->by = b->y;
    t->long_dx = (c->x - a->x) * fast_recip(c->y - a->y);
    t->top_dx = b->y > a->y ? (b->x - a->x) * fast_recip(b->y - a->y) : 0.0f;
    t->bot_dx = c->y > b->y ? (c->x - b->x) * fast_recip(c->y - b->y) : 0.0f;
    t->q0 = a->q;
    if (t->affine) { t->uq0 = a->uq * fast_recip(a->q); t->vq0 = a->vq * fast_recip(a->q); }
    else { t->uq0 = a->uq; t->vq0 = a->vq; }
    t->depth_scale = depth_scale;
    t->y0 = (int16_t)y0; t->y2 = (int16_t)y2;
    t->paint = paint;
    t->long_left = area > 0;
    t->level = (uint8_t)level;
    t->key = (uint8_t)(255u - (depth16(fmaxf(a->q, fmaxf(b->q, c->q)) * depth_scale) >> 8));   /* near first */
    /* Fixed-point rows (raster_rows_fixed): the edges in 16.16, and for an affine triangle u and v too. */
    {
        const float xmax = fmaxf(fabsf(a->x), fmaxf(fabsf(b->x), fabsf(c->x)));
        int ok = xmax < 8192.0f;
        if (ok && t->affine) {   /* u and v in 16.16 texels (level 0, the largest) */
            const float m = fmaxf(fmaxf(fabsf(t->dudx), fabsf(t->dudy)), fmaxf(fabsf(t->dvdx), fabsf(t->dvdy)));
            ok = m * 65536.0f < 268435456.0f;   /* 2^28 */
        }
        if (ok) t->affine |= 2;
    }
    return 1;
}

/* The frame's paint of a binding: consecutive faces share one (faces are sorted by material). */
/* The frame's paint index of a binding, set up by the first core that asks; -1 when it cannot be drawn. The two
   cores build their lists at the same time: a slot is claimed with a compare-and-swap (internal RAM), the paint
   set up, then the index published; a core that finds a slot being set up waits for it (a few hundred cycles). */
static uint32_t g_dropped_paints, g_dropped_tris;   /* faces lost to the caps (reported, sw3d_report_drops) */
static int paint_index(const materials_t *m, const nm_material_binding_t *b, unsigned core)
{
    (void)core;
    const uint32_t key1 = ((m == &G.world ? 1u : 0u) << 16 | (uint32_t)(b - m->pack.bindings)) + 1u;
    unsigned h = (key1 * 2654435761u) >> 21;   /* 11 bits: PAINT_HASH */
    for (unsigned probe = 0; probe < PAINT_HASH; ++probe, h = (h + 1u) & (PAINT_HASH - 1u)) {
        uint32_t v = __atomic_load_n(g_paint_hash + h, __ATOMIC_ACQUIRE);
        if (!v) {
            if (__atomic_compare_exchange_n(g_paint_hash + h, &v, key1 << 12 | 0xfffu, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                const uint32_t i = __atomic_fetch_add(&g_paint_count, 1u, __ATOMIC_RELAXED);
                const int ok = i < PAINT_CAP && paint_setup(m, b, paint_at(i));
                if (i >= PAINT_CAP) ++g_dropped_paints;
#ifdef SW3D_PROFILE
                ++cnt_pset[core];
#endif
                __atomic_store_n(g_paint_hash + h, key1 << 12 | (ok ? i : 0xffeu), __ATOMIC_RELEASE);
                return ok ? (int)i : -1;
            }
            /* lost the race: v now holds the winner's claim */
        }
        if ((v >> 12) == key1) {
            while ((v & 0xfffu) == 0xfffu) v = __atomic_load_n(g_paint_hash + h, __ATOMIC_ACQUIRE);
            return (v & 0xfffu) == 0xffeu ? -1 : (int)(v & 0xfffu);
        }
    }
    return -1;
}

/* The paint of a binding: consecutive faces share one (faces are sorted by material). */
static const paint_t *paint_for(list_t *L, const materials_t *m, const nm_material_binding_t *b)
{
    if (b != L->last_binding) { L->last_paint = paint_index(m, b, (unsigned)(L - g_list)); L->last_binding = b; }
    return L->last_paint < 0 ? NULL : paint_at((unsigned)L->last_paint);
}

/* ------------------------------------------------------------------ faces */

typedef struct { float x, y, z, u, v; } vvert_t;   /* view space, u v in texels */

static int clip_near(const vvert_t *in, int n, vvert_t *out)
{
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const vvert_t *a = in + i, *b = in + (i + 1) % n;
        const int ain = a->z >= NEAR_Z, bin = b->z >= NEAR_Z;
        if (ain) out[m++] = *a;
        if (ain != bin) {
            const float t = (NEAR_Z - a->z) * fast_recip(b->z - a->z);
            out[m++] = (vvert_t){ a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, NEAR_Z,
                                  a->u + (b->u - a->u) * t, a->v + (b->v - a->v) * t };
        }
    }
    return m;
}

/* One face with its paint: transform, cut at the near plane, project, fan, rasterise. */
static void draw_face(list_t *L, const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *inst, const nm_face_t *face,
                      const paint_t *paint, float depth_scale)
{
    const float *m = inst->matrix;
    const float fx = inst->focus[0], fy = inst->focus[1];
    const float cx = (float)inst->center[0], cy = 384.0f - (float)inst->center[1];
    vvert_t v[4], clipped[8];
    int all_near = 1;
#ifdef SW3D_PROFILE
    const uint32_t xf0_ = CYCLES();
#endif
    for (unsigned k = 0; k < face->count; ++k) {
        const nm_vertex_t *src = pack->vertices + mesh->vertex_first + face->indices[k];
        const float *p = src->position;
        v[k].x = p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9];
        v[k].y = p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10];
        v[k].z = p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11];
        v[k].u = src->u * 0.125f;
        v[k].v = src->v * 0.125f;
        if (v[k].z >= 1.0f / NM_SKY_DEPTH) return;   /* beyond the sky (constant folded) */
        all_near &= v[k].z >= NEAR_Z;
    }
    const vvert_t *poly = v;
    int n = face->count;
    if (!all_near) { n = clip_near(v, n, clipped); poly = clipped; if (n < 3) return; }
    svert_t s[8];
    for (int k = 0; k < n; ++k) {
        const float q = fast_recip(poly[k].z);
        s[k].x = (cx + poly[k].x * fx * q) * XS;
        s[k].y = (cy - poly[k].y * fy * q) * YS;
        s[k].q = q;
        s[k].uq = poly[k].u * q;
        s[k].vq = poly[k].v * q;
    }
    ++L->faces;
#ifdef SW3D_PROFILE
    const uint32_t mp0_ = CYCLES(); cyc_xf[L - g_list] += mp0_ - xf0_;
#endif
    /* mip level: texels per pixel from the polygon's areas in texture and on screen (one level per face) */
    unsigned level = 0;
#ifdef SW3D_NO_MIP
    if (0) {
#else
    if (paint->texture && paint->texture->levels > 1) {
#endif
        float sa = 0, ta = 0;
        for (int k = 1; k + 1 < n; ++k) {
            sa += fabsf((s[k].x - s[0].x) * (s[k + 1].y - s[0].y) - (s[k + 1].x - s[0].x) * (s[k].y - s[0].y));
            ta += fabsf((poly[k].u - poly[0].u) * (poly[k + 1].v - poly[0].v) - (poly[k + 1].u - poly[0].u) * (poly[k].v - poly[0].v));
        }
        if (sa > 0.0f && ta > sa) {
            /* floor(log2(ta / sa)) from the bits, without a divide: the exponents' difference, less one when
               ta's mantissa is below sa's */
            const union { float f; uint32_t u; } bt = { ta }, bs = { sa };
            const int log2r = (int)(bt.u >> 23) - (int)(bs.u >> 23) - ((bt.u & 0x7fffffu) < (bs.u & 0x7fffffu));
            level = (unsigned)((log2r + 1) / 2);   /* the nearer level: fewer texels fetched per pixel */
        }
    }
#ifdef SW3D_PROFILE
    const uint32_t ts0_ = CYCLES(); cyc_mip[L - g_list] += ts0_ - mp0_;
#endif
    if (L->tri_count + (unsigned)n - 2u > L->cap) g_dropped_tris += L->tri_count + (unsigned)n - 2u - L->cap;
    for (int k = 1; k + 1 < n && L->tri_count < L->cap; ++k)
        if (
#ifdef SW3D_PROFILE
            (++cnt_setup[L - g_list], 1) &&
#endif
            tri_setup(L->tris + L->tri_count, s, s + k, s + k + 1, (uint16_t)L->last_paint, level, depth_scale)) {
            L->keys[L->tri_count] = L->tris[L->tri_count].key;
            ++L->tri_count;
        }
#ifdef SW3D_PROFILE
    cyc_setup[L - g_list] += CYCLES() - ts0_;
#endif
}

static float face_depth_scale(const nm_material_t *mat, const nm_face_t *face, int classes)
{
    float s = 1.0f + (float)face->layer * NM_LAYER_DEPTH_STEP;
    if (classes && (mat->padding[0] & NM_SCENERY_SURFACE)) s *= G.road_depth;
    return s;
}

/* rear: the face's supplied normal points away from the eye (view space n . p2 < 0). */
static int face_rear(const nm_pack_t *pack, const nm_mesh_t *mesh, const nm_instance_t *inst, const nm_face_t *face)
{
    const float *m = inst->matrix, *nn = face->normal;
    const float *p = pack->vertices[mesh->vertex_first + face->indices[2]].position;
    float n[3], q[3];
    for (unsigned a = 0; a < 3; ++a) {
        n[a] = nn[0] * m[a] + nn[1] * m[3 + a] + nn[2] * m[6 + a];
        q[a] = p[0] * m[a] + p[1] * m[3 + a] + p[2] * m[6 + a] + m[9 + a];
    }
    return n[0] * q[0] + n[1] * q[1] + n[2] * q[2] < 0;
}

/* Whether a box (min xyz, max xyz) under matrix m can show in the instance's view: not all eight corners
   outside one side of the 496x384 view (or nearer than the near plane, or beyond the sky). */
static int box_visible(const float bounds[6], const nm_instance_t *inst)
{
    const float *m = inst->matrix;
    const float fx = inst->focus[0], fy = inst->focus[1];
    const float cx = (float)inst->center[0], cy = 384.0f - (float)inst->center[1];
    unsigned all = 0x3f;   /* bit per plane: every corner so far outside it */
    for (unsigned k = 0; k < 8; ++k) {
        const float px = bounds[k & 1 ? 3 : 0], py = bounds[k & 2 ? 4 : 1], pz = bounds[k & 4 ? 5 : 2];
        const float x = px * m[0] + py * m[3] + pz * m[6] + m[9];
        const float y = px * m[1] + py * m[4] + pz * m[7] + m[10];
        const float z = px * m[2] + py * m[5] + pz * m[8] + m[11];
        unsigned out = 0;
        if (z < NEAR_Z) out |= 1;
        if (z > SW3D_FAR) out |= 2;
        if (x * fx + cx * z < 0) out |= 4;                     /* left of the view */
        if ((496.0f - cx) * z - x * fx < 0) out |= 8;          /* right */
        if (cy * z - y * fy < 0) out |= 16;                    /* above */
        if (y * fy + (384.0f - cy) * z < 0) out |= 32;         /* below */
        all &= out;
        if (!all) return 1;
    }
    return 0;
}

static unsigned g_group_count;

/* The level of a level-of-detail group for this instance (meshpack.h): level n+1 instead of n beyond the view-space
   depth switch_depth[n] of the group's centre, authored for a vertical focus of NM_LOD_REFERENCE_FOCUS. No
   hysteresis here (one frame's choice). */
static unsigned lod_pick(const nm_lod_group_t *g, const nm_instance_t *inst)
{
    const float *m = inst->matrix;
    const float depth = (g->centre[0] * m[2] + g->centre[1] * m[5] + g->centre[2] * m[8] + m[11]) *
                        (NM_LOD_REFERENCE_FOCUS * fast_recip(inst->focus[1]));
    unsigned level = 0;
    while (level + 1u < g->levels && g->switch_depth[level] > 0.0f && depth > g->switch_depth[level]) ++level;
    return level;
}

/* part: the core; it takes the faces of every other group of 8 (f >> 3 & 1 == part) */
static uint32_t dbg_world[5];   /* diagnostics: instances, in view, faces, bound, painted */
static void draw_world_mesh(list_t *L, unsigned part, const nm_pack_t *pack, const nm_instance_t *inst, int object)
{
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    int vis_;
    ++dbg_world[0];
    PROF(cyc_mbox, part, vis_ = box_visible(mesh->bounds, inst));
    dbg_world[1] += vis_ != 0;
#ifdef SW3D_PROFILE
    ++cnt_inst[part]; cnt_inst_vis[part] += vis_ != 0;
#endif
    if (!vis_) return;
    const unsigned flags = nm_pack_flags(pack), classes = flags & NM_PACK_SCENERY_CLASSES;
    if (G.face_lod && L->lod_level && mesh->padding[4] >> 16) {   /* this mesh's level-of-detail groups: not picked yet */
        const unsigned first = mesh->padding[4] & 0xffffu, count = mesh->padding[4] >> 16;
        if (first + count <= G.lod_count) memset(L->lod_level + first, 0xff, count);
    }
    uint8_t *const group_seen = L->group_seen;
    const int grouped = nm_pack_has_groups(pack) && group_seen;
    if (grouped) {   /* this mesh's groups are untested for this instance */
        const unsigned first = mesh->padding[0], count = mesh->padding[1];   /* meshpack.h: DNM3+ group first, count */
        if (first + count <= g_group_count) memset(group_seen + first, 0, count);
    }
    for (unsigned f = 0; f < mesh->face_count; ++f) {
        if (((f >> 3) & 1u) != part) { f |= 7u; continue; }
#ifdef SW3D_PROFILE
        ++cnt_face_iter[part];
#endif
        const unsigned abs_face = mesh->face_first + f;
        if (grouped) {
            const unsigned g = pack->faces[abs_face].group;
            if (g < g_group_count) {
                if (!group_seen[g]) {
#ifdef SW3D_PROFILE
                    ++cnt_grp[part];
#endif
                    PROF(cyc_grp, part, group_seen[g] = box_visible(nm_pack_group_bounds(pack, g), inst) ? 1 : 2);
                }
                if (group_seen[g] == 2) continue;
            }
        }
        if (G.face_lod && G.face_lod[abs_face]) {   /* a level-of-detail group: draw the range of its level */
            const unsigned g = G.face_lod[abs_face] - 1u;
            if (G.face_level[abs_face] == 0xff) continue;
            if (L->lod_level[g] == 0xff) PROF(cyc_lod, part, L->lod_level[g] = (uint8_t)lod_pick(pack->lod_groups + g, inst));
            if (L->lod_level[g] != G.face_level[abs_face]) continue;
        }
        const nm_face_t *face = pack->faces + abs_face;
        const nm_material_t *mat = pack->materials + face->material;
        ++dbg_world[2];
        if (classes) {
            const unsigned stage = mat->padding[0] & NM_SCENERY_STAGE_MASK;
            if (stage == NM_SCENERY_STAGE_CHECKER || (stage == NM_SCENERY_STAGE_OBJECTS && !object)) continue;
        }
        int rear;
        PROF(cyc_rear, part, rear = face_rear(pack, mesh, inst, face));
        if (rear && !(face->attribute & (1u << 17))) continue;
#ifdef SW3D_PROFILE
        const uint32_t bd0_ = CYCLES();
#endif
        uint16_t *slot = G.world_binding + 2u * abs_face + (rear ? 1u : 0u);
        if (!*slot) {
            const unsigned luma = rear ? face->padding : face->reserved;
            const nm_material_binding_t *b = nm_material_find(&G.world.pack, mat->header, luma);
            *slot = b ? (uint16_t)(b - G.world.pack.bindings + 1) : 0xffffu;
#ifdef SW3D_PROFILE
            ++cnt_find[part];
#endif
        }
        if (*slot == 0xffffu) continue;
        ++dbg_world[3];
        const nm_material_binding_t *b = G.world.pack.bindings + (*slot - 1);
        const int transl_ = nm_material_translucent(b->header);
#ifdef SW3D_PROFILE
        cyc_bind[part] += CYCLES() - bd0_;
#endif
        if (transl_) continue;
        const paint_t *paint;
        PROF(cyc_paint, part, paint = paint_for(L, &G.world, b));
        if (!paint) continue;
        PROF(cyc_face, part, draw_face(L, pack, mesh, inst, face, paint, face_depth_scale(mat, face, (int)classes)));
    }
}

static uint32_t *g_car_binding;   /* per car face: luma << 16 | binding + 1 of the last lookup (0: none) */

static uint32_t dbg_car[6];   /* diagnostics: instances, meshes in view, faces, lit, bound, painted */
static void draw_car_mesh(list_t *L, unsigned part, const nm_pack_t *pack, const nm_instance_t *inst)
{
    ++dbg_car[0];
    if (inst->mesh >= pack->header->meshes) return;
    const nm_mesh_t *mesh = pack->meshes + inst->mesh;
    if (!box_visible(mesh->bounds, inst)) return;
    ++dbg_car[1];
    for (unsigned f = 0; f < mesh->face_count; ++f) {
        if (((f >> 3) & 1u) != part) { f |= 7u; continue; }
        const nm_face_t *face = pack->faces + mesh->face_first + f;
        if (!(face->attribute & (3u << 8))) continue;
        ++dbg_car[2];
        const int light = face_light(pack, mesh, inst, face, 0);
        if (light < 0) continue;
        ++dbg_car[3];
        if ((light & 256) && !(face->attribute & (1u << 17))) continue;
        const nm_material_t *mat = pack->materials + face->material;
        if (mat->header[0] & 0x8000u) continue;   /* screen checker: the smoke, not drawn yet */
        const unsigned luma = (unsigned)light & 255u, abs_face = mesh->face_first + f;
        const nm_material_binding_t *b;
#ifdef SW3D_NO_CAR_CACHE
        uint32_t *cache = NULL;
#else
        uint32_t *cache = g_car_binding ? g_car_binding + abs_face : NULL;
#endif
        if (cache && *cache && (*cache >> 16) == luma) b = G.car.pack.bindings + ((*cache & 0xffffu) - 1u);
        else {
            b = nm_material_find(&G.car.pack, mat->header, luma);
            if (cache && b) *cache = luma << 16 | (uint32_t)(b - G.car.pack.bindings + 1);
        }
        if (!b || nm_material_translucent(b->header)) continue;
        ++dbg_car[4];
        const paint_t *paint = paint_for(L, &G.car, b);
        if (!paint) continue;
        ++dbg_car[5];
        PROF(cyc_face, part, draw_face(L, pack, mesh, inst, face, paint, 1.0f + (float)face->layer * NM_LAYER_DEPTH_STEP));
    }
}

/* ------------------------------------------------------------------ frame entry points */

static int frame_wanted(void)
{
    return G.pixels && G.depth && (!sw3d_frame_wanted || sw3d_frame_wanted(G.frame));
}

/* The sky comes first in practice_prepare's order? No: objects are queued first, the sky after. Both
   draw at once here, so the sky is drawn under everything with depth NM_SKY_DEPTH. */
static void sky_draw(uint16_t heading, uint16_t scroll, int parity)
{
    nm_sky_rect_t rects[NM_SKY_RECTS];
    const unsigned n = nm_sky_rects_rows(heading, scroll, G.sky_rows ? G.sky_rows : NM_SKY_ROWS, rects);
    /* panorama texel per screen pixel: 496/284 across, 384/216 down, stepped in 16.16 (no divide per pixel) */
    const uint32_t step_x = (uint32_t)(65536.0f * 496.0f / (float)W), step_y = (uint32_t)(65536.0f * 384.0f / (float)H);
    for (unsigned i = 0; i < n; ++i) {
        const nm_sky_rect_t *r = rects + i;
        const int x0 = (int)(r->x * XS), x1 = (int)((r->x + r->width) * XS);
        const int y0 = (int)(r->y * YS), y1 = (int)((r->y + r->height) * YS);
        const int flat = r->page >= 4 || !G.sky_pages;
        const uint16_t fill = rgb565(G.sky_fill[r->page == NM_SKY_GROUND ? 1 : 0]);
        const uint8_t *page = flat ? NULL : G.sky_pages + (size_t)r->page * NM_SKY_TEXTURE_BYTES + 32;
        /* texel at the first pixel centre of the rect, in 16.16. The rect's edges are scaled down to whole pixels,
           so the first pixel's centre can fall just before the rect: clamped to its first column (not wrapped
           to the page's last one) */
        const int32_t u_in = (int32_t)(((float)x0 + 0.5f) * (65536.0f * 496.0f / (float)W)) - (int32_t)(r->x * 65536u);
        const uint32_t u_start = (uint32_t)(r->u * 65536u) + (uint32_t)(u_in > 0 ? u_in : 0);
        for (int y = band_first(y0, parity); y < y1 && y < H; y += RS) {
            uint16_t *row = G.pixels + y * W;
            uint16_t *zrow = G.depth + y * W;
            if (flat) {
                for (int x = x0; x < x1 && x < W; ++x) if (!zrow[x]) row[x] = fill;
                continue;
            }
            /* the row's panorama line within the rect, clamped to it like u: the first and last screen rows can
               sample just outside, which read texture rows beyond the sky (an olive line across it) */
            int dv = (int)(((uint32_t)y * step_y + step_y / 2) >> 16) - (int)r->y;
            dv = dv < 0 ? 0 : dv >= (int)r->height ? (int)r->height - 1 : dv;
            const unsigned v = (r->v + (unsigned)dv) & 511;
            const uint32_t twy = tw_y[v];
            uint32_t u = u_start;
            for (int x = x0; x < x1 && x < W; ++x, u += step_x) {
                if (zrow[x]) continue;
                const uint32_t tw = tw_x[(u >> 16) & 511] | twy;
                const uint8_t e = page[2048 + (tw >> 2)];
                const uint8_t *c = page + 2 * (e * 4 + (tw & 3));
                row[x] = (uint16_t)(c[0] | c[1] << 8);
            }
        }
    }
}

/* One core's share of the picture: every triangle of the list on its rows, then the sky under them. */
static uint16_t g_order[SETS][TRI_SET_CAP];   /* both lists sorted nearest first (internal RAM: read per triangle) */
static uint8_t g_keys[TRI_SET_CAP];
static unsigned g_set_count[SETS];
static uint16_t g_set_sky[SETS][2];
static void sort_near_first(unsigned set)
{
    static uint16_t count[257];
    memset(count, 0, sizeof count);
    for (unsigned l = 0; l < 2; ++l)
        for (unsigned i = 0; i < g_list[l].tri_count; ++i) ++count[g_list[l].keys[i] + 1u];
#ifdef SW3D_NO_SORT
    memset(count, 0, sizeof count);   /* measurement: list order (one key) */
    for (unsigned l = 0; l < 2; ++l) count[1] += g_list[l].tri_count;
    for (unsigned l = 0; l < 2; ++l) memset(g_list[l].keys, 0, g_list[l].tri_count);
#endif
    for (unsigned k = 1; k <= 256; ++k) count[k] += count[k - 1];
    for (unsigned l = 0; l < 2; ++l)
        for (unsigned i = 0; i < g_list[l].tri_count; ++i) g_order[set][count[g_list[l].keys[i]]++] = (uint16_t)(l << 15 | i);
    G.tri_count = g_set_count[set] = g_list[0].tri_count + g_list[1].tri_count;
}

#ifdef SW3D_PROFILE
static uint32_t cyc_band[2], cyc_sky[2];
#endif
/* noclone: a clone (fill_band.constprop.0) would miss linker.lf's IRAM placement and run from flash */
static __attribute__((noinline, noclone)) void fill_band(int parity, unsigned set)
{
#ifdef SW3D_PROFILE
    const uint32_t b0_ = CYCLES();
#endif
    const uint16_t *order = g_order[set];
    const pool_t *pl = g_pool + set;
    for (unsigned i = 0, n = g_set_count[set]; i < n; ++i) {
        const tri_t *t = g_tris[set][order[i] >> 15] + (order[i] & 0x7fffu);
        raster_body(t, paint_in(pl, t->paint), parity);
    }
#ifdef SW3D_PROFILE
    const uint32_t s0_ = CYCLES();
#endif
    sky_draw(g_set_sky[set][0], g_set_sky[set][1], parity);
#ifdef SW3D_PROFILE
    cyc_sky[parity] += CYCLES() - s0_;
    cyc_band[parity] += CYCLES() - b0_;
#endif
}

/* One core's half of the geometry: its faces of every world instance and every car part, into its list. */
static void build_part(unsigned part)
{
#ifdef SW3D_PROFILE
    const uint32_t b0_ = CYCLES();
#endif
    nm_practice_scene_t *scene = G.scene;
    list_t *L = g_list + part;
    L->tri_count = L->faces = 0; L->last_binding = NULL; L->last_paint = -1;
    if (scene->world_sources)
        for (unsigned i = 0; i < scene->world_header.instances; ++i)
            draw_world_mesh(L, part, &scene->world, scene->world.instances + i,
                            (scene->world.instances[i].padding[0] & NM_OBJECT_INSTANCE) != 0);
#ifdef SW3D_PROFILE
    const uint32_t c0_ = CYCLES();
#endif
    for (unsigned i = 0; i < G.car_part_count && i < scene->model.header->instances; ++i)
        draw_car_mesh(L, part, &scene->model, scene->model.instances + i);
#ifdef SW3D_PROFILE
    cyc_cars[part] += CYCLES() - c0_;
    cyc_build[part] += CYCLES() - b0_;
#endif
}

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
static TaskHandle_t band_task;
static SemaphoreHandle_t band_done;
static volatile int band_phase;   /* 0: build part 1; 1: fill the odd rows; 2 (PIPE): draw set band_set, present */
static volatile unsigned band_set;
static uint32_t band_cycles[2];
static void pipe_draw(unsigned set);
/* Core 1: when core 0 asks (task notification), its half of the geometry, the odd rows, or (PIPE) a whole
   picture; then band_done. */
static void band_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t t0 = CYCLES();
        if (band_phase == 0) build_part(1);
        else if (band_phase == 1) fill_band(1, 0);
        else pipe_draw(band_set);
        band_cycles[1] = CYCLES() - t0;
        xSemaphoreGive(band_done);
    }
}
static void band_start(void)
{
    if (!band_task) {
        band_done = xSemaphoreCreateBinary();
        xTaskCreatePinnedToCore(band_worker, "sw3d_band", 6144, NULL, configMAX_PRIORITIES - 4, &band_task, 1);
    }
}
#if PIPE
static int pipe_pending;
/* Core 0: wait for the picture core 1 is drawing (if any). */
static void pipe_wait(void)
{
    if (pipe_pending) { xSemaphoreTake(band_done, portMAX_DELAY); pipe_pending = 0; }
}
/* Core 1: one whole picture from a set, then hand it over. */
static uint32_t g_set_tag[SETS];
static void pipe_draw(unsigned set)
{
    const uint32_t t0 = now_us();
    memset(G.depth, 0, sizeof(uint16_t) * W * H);
    fill_band(1, set);
    G.stats.sky_us = now_us() - t0;
    G.stats.triangles = g_set_count[set];
    ++G.stats.frames;
    if (sw3d_overlay) sw3d_overlay(G.pixels, G.depth, g_set_tag[set]);
    if (sw3d_frame_done) sw3d_frame_done(G.picture, G.frame);
    G.last_done_us = now_us();
}
#else
static void pipe_draw(unsigned set) { (void)set; }
#endif
static void both_cores(int phase)
{
    band_start();
    band_phase = phase;
#ifdef SW3D_ONE_CORE_FILL
    if (phase) {   /* measurement: core 0 fills both parities, core 1 idle */
        const uint32_t t0 = CYCLES();
        fill_band(0, 0); fill_band(1, 0);
        band_cycles[0] = band_cycles[1] = CYCLES() - t0;
        G.stats.raster_cycles = band_cycles[0];
        return;
    }
#endif
    xTaskNotifyGive(band_task);
    const uint32_t t0 = CYCLES();
    if (phase == 0) build_part(0); else fill_band(0, 0);
    band_cycles[0] = CYCLES() - t0;
    xSemaphoreTake(band_done, portMAX_DELAY);
    if (phase) G.stats.raster_cycles = band_cycles[0] > band_cycles[1] ? band_cycles[0] : band_cycles[1];
}
#else
static void both_cores(int phase)
{
    if (phase == 0) { build_part(0); build_part(1); }
    else { fill_band(0, 0); if (!ALL_ROWS) fill_band(1, 0); }
}
#endif

/* The pipeline idle: no picture is being drawn (textures may go). */
void sw3d_wait_idle(void)
{
#if PIPE
    pipe_wait();
#endif
}

/* Without the pipeline, after the frame's 2D layer is captured: the picture drawn in this frame, presented. */
void sw3d_finish(void)
{
    if (!g_done_pending) return;
    g_done_pending = 0;
    if (sw3d_overlay) sw3d_overlay(G.pixels, G.depth, g_done_tag);
    if (sw3d_frame_done) sw3d_frame_done(G.picture, G.frame);
    G.last_done_us = now_us();
}

/* A picture of the 2D layer alone (the title, the select screens): black, the layer, presented. */
void sw3d_present_alone(void)
{
    if (!G.picture || !G.depth) return;
    sw3d_wait_idle();
    memset(G.picture, 0, sizeof(uint16_t) * W * SW3D_H_PX);
    if (sw3d_overlay) sw3d_overlay(G.pixels, G.depth, 0);
    if (sw3d_frame_done) sw3d_frame_done(G.picture, ++G.frame);
}

int nm_render_enqueue_objects(const unsigned *car_parts, unsigned cars, unsigned player)
{
    (void)player;
    ++G.frame;
#ifdef SW3D_DIAG   /* diagnostics: what the race hands in and what of it is drawn, every 100 pictures */
    if ((G.frame % 100u) == 0 && G.scene && !G.scene->attract_active && G.scene->race.count)
        printf("sw3d: race view %u, camera mode %u, %u cars handed in, car parts %u (header %u); car draws: %u instances, %u in view, %u faces, %u lit, %u bound, %u painted\n",
               (unsigned)G.scene->race.view, (unsigned)G.scene->race.cars[0].car.camera_mode, cars,
               car_parts && cars ? car_parts[cars] : 0u, (unsigned)G.scene->model.header->instances,
               dbg_car[0], dbg_car[1], dbg_car[2], dbg_car[3], dbg_car[4], dbg_car[5]);
    if ((G.frame % 100u) == 0) {
        if (G.scene) printf("sw3d: course %u world draws: %u instances, %u in view, %u faces, %u bound; triangles %u\n",
                            G.scene->course, dbg_world[0], dbg_world[1], dbg_world[2], dbg_world[3], G.tri_count);
        memset(dbg_car, 0, sizeof dbg_car); memset(dbg_world, 0, sizeof dbg_world);
    }
#endif
    if ((G.frame % 100u) == 0 && (g_dropped_paints || g_dropped_tris)) {
        printf("sw3d: the caps dropped %u faces' paints and %u triangles in the last 100 pictures\n",
               (unsigned)g_dropped_paints, (unsigned)g_dropped_tris);
        g_dropped_paints = g_dropped_tris = 0;
    }
    if (!frame_wanted() || !g_tris[0][0]) return 1;
    nm_practice_scene_t *scene = G.scene;
    const uint32_t t0 = now_us();
    G.stats.start_us = t0;
    G.stats.between_us = G.last_done_us ? t0 - G.last_done_us : 0;   /* simulation, scene set-up, presenting */
    G.car_part_count = car_parts && cars ? car_parts[cars] : scene->model.header->instances;
    memset(g_paint_hash, 0, sizeof g_paint_hash); g_paint_count = 0;
    for (unsigned l = 0; l < 2; ++l) g_list[l].tris = g_tris[g_build_set][l];
#if PIPE
    {   /* core 0 builds the whole list while core 1 draws the last picture; then core 1 draws this one */
        const unsigned set = g_build_set;
        g_list[0].tris = g_tris[set][0]; g_list[0].keys = g_keys; g_list[0].cap = TRI_SET_CAP;
        build_part(0);
        const unsigned n0 = g_list[0].tri_count;
        g_tris[set][1] = g_list[1].tris = g_tris[set][0] + n0; g_list[1].keys = g_keys + n0; g_list[1].cap = TRI_SET_CAP - n0;
        build_part(1);
        G.stats.faces = g_list[0].faces + g_list[1].faces;
        sort_near_first(set);
        g_set_sky[set][0] = (uint16_t)scene->camera_heading; g_set_sky[set][1] = scene->intro.sky_scroll;
        const uint32_t t1 = now_us();
        G.stats.world_us = t1 - t0;
        band_start();
        pipe_wait();
        G.stats.wait_us = now_us() - t1;
        g_set_tag[set] = sw3d_posted + 1u;
        band_set = set; band_phase = 2; pipe_pending = 1;
        sw3d_posted = g_set_tag[set];
        xTaskNotifyGive(band_task);
        g_build_set ^= 1u;
        return 1;
    }
#endif
    both_cores(0);
    G.stats.faces = g_list[0].faces + g_list[1].faces;
    const uint32_t t1 = now_us();
    const uint32_t t2 = t1;
    G.sky_heading = (uint16_t)scene->camera_heading; G.sky_scroll = scene->intro.sky_scroll;
    memset(G.depth, 0, sizeof(uint16_t) * W * H);
    sort_near_first(0);
    g_set_sky[0][0] = G.sky_heading; g_set_sky[0][1] = G.sky_scroll;
    both_cores(1);
#ifdef SW3D_OVERDRAW
    printf("overdraw frame %u: %llu px visited (%.2f per picture pixel), %llu written, %llu hidden (%.0f%%); rows %llu, "
           "hidden %llu (%.0f%%, %llu px); triangles %llu, hidden %llu (%.0f%%, %llu px)\n", G.frame, od_px,
           (double)od_px / (W * H), od_written, od_px - od_written, 100.0 * (double)(od_px - od_written) / (double)od_px,
           od_rows, od_hidden_rows, 100.0 * (double)od_hidden_rows / (double)od_rows, od_hidden_row_px, od_tris,
           od_hidden_tris, 100.0 * (double)od_hidden_tris / (double)od_tris, od_hidden_tri_px);
    od_px = od_written = od_rows = od_hidden_rows = od_hidden_row_px = od_tris = od_hidden_tris = od_hidden_tri_px = 0;

#endif
    const uint32_t t3 = now_us();
    /* world_us and cars_us: building the triangle list; sky_us: both cores filling the picture */
    G.stats.world_us = t1 - t0; G.stats.cars_us = t2 - t1; G.stats.sky_us = t3 - t2;
    G.stats.triangles = G.tri_count;
    G.stats.rows = cnt_rows[0] + cnt_rows[1]; G.stats.pixels = cnt_px[0] + cnt_px[1]; G.stats.drawn = cnt_drawn[0] + cnt_drawn[1];
    cnt_rows[0] = cnt_rows[1] = cnt_px[0] = cnt_px[1] = cnt_drawn[0] = cnt_drawn[1] = 0;
    ++G.stats.frames;
#ifdef SW3D_PROFILE
    if (G.stats.frames % 50 == 1)
        printf("sw3d profile core0 (us): band %u, triangles %u (prologues %u, kernel %u, spans %u, row iterations %u, rows %u), sky %u\n",
               (unsigned)(cyc_band[0] / 240), (unsigned)(cyc_tri[0] / 240), (unsigned)(cyc_pro[0] / 240),
               (unsigned)(cyc_kernel[0] / 240), (unsigned)cnt_spans[0], (unsigned)cnt_iter[0], (unsigned)cnt_rows[0], (unsigned)(cyc_sky[0] / 240));
    if (G.stats.frames % 50 == 1)
        printf("sw3d profile core0 fixed rows: %u of %u triangles, %u us (first touch %u, prologue %u, kernel %u), %u row iterations, %u of %u pixels; band pixels %u, drawn %u\n",
               (unsigned)cnt_fix_tri[0], (unsigned)cnt_tri[0], (unsigned)(cyc_fix[0] / 240), (unsigned)(cyc_fix_touch[0] / 240),
               (unsigned)(cyc_fix_pro[0] / 240), (unsigned)(cyc_fix_k[0] / 240), (unsigned)cnt_fix_iter[0],
               (unsigned)cnt_fix_px[0], (unsigned)G.stats.pixels / 2, (unsigned)G.stats.pixels, (unsigned)G.stats.drawn);
    for (int c = 0; c < 2; ++c) cyc_band[c] = cyc_tri[c] = cyc_kernel[c] = cyc_sky[c] = cyc_pro[c] = cnt_spans[c] = cnt_iter[c] = 0;
    if (G.stats.frames % 50 == 1)
        printf("sw3d profile core0 build: %u us (cars %u), faces iterated %u, draw_face %u us (tri_setup %u us, %u calls), list %u faces %u triangles %u paints\n",
               (unsigned)(cyc_build[0] / 240), (unsigned)(cyc_cars[0] / 240), (unsigned)cnt_face_iter[0], (unsigned)(cyc_face[0] / 240),
               (unsigned)(cyc_setup[0] / 240), (unsigned)cnt_setup[0], g_list[0].faces, g_list[0].tri_count, (unsigned)g_paint_count);
    if (G.stats.frames % 50 == 1)
        printf("sw3d profile core0 build detail (us): mesh boxes %u (%u instances, %u visible), group boxes %u (%u), lod %u, rear %u, binding %u (%u finds), paint %u (%u set up), transform %u, mip %u\n",
               (unsigned)(cyc_mbox[0] / 240), (unsigned)cnt_inst[0], (unsigned)cnt_inst_vis[0], (unsigned)(cyc_grp[0] / 240), (unsigned)cnt_grp[0],
               (unsigned)(cyc_lod[0] / 240), (unsigned)(cyc_rear[0] / 240), (unsigned)(cyc_bind[0] / 240), (unsigned)cnt_find[0],
               (unsigned)(cyc_paint[0] / 240), (unsigned)cnt_pset[0], (unsigned)(cyc_xf[0] / 240), (unsigned)(cyc_mip[0] / 240));
    for (int c = 0; c < 2; ++c) cyc_mbox[c] = cnt_inst[c] = cnt_inst_vis[c] = cyc_grp[c] = cnt_grp[c] = cyc_lod[c] = cyc_rear[c] = cyc_bind[c] = cnt_find[c] = cyc_paint[c] = cnt_pset[c] = cyc_xf[c] = cyc_mip[c] = 0;
    for (int c = 0; c < 2; ++c) cyc_build[c] = cyc_face[c] = cyc_setup[c] = cyc_cars[c] = cnt_face_iter[c] = cnt_setup[c] = 0;
    for (int c = 0; c < 2; ++c) cyc_fix_pro[c] = cyc_fix_touch[c] = cyc_fix_k[c] = 0;
    for (int c = 0; c < 2; ++c) cyc_fix[c] = cnt_fix_tri[c] = cnt_fix_iter[c] = cnt_fix_px[c] = cnt_tri[c] = 0;
#endif
#if defined(SW3D_VARIANTS) && defined(SW3D_PROFILE)
    { extern unsigned sw3d_var_px[16]; printf("variant pixels:"); for (int i = 0; i < 16; ++i) printf(" %u", sw3d_var_px[i]); printf("\n"); memset(sw3d_var_px, 0, sizeof sw3d_var_px); }
#endif
    const uint32_t tag = ++sw3d_posted;
    if (sw3d_defer_done) { g_done_pending = 1; g_done_tag = tag; return 1; }
    if (sw3d_overlay) sw3d_overlay(G.pixels, G.depth, tag);
    if (sw3d_frame_done) sw3d_frame_done(G.picture, G.frame);
    G.last_done_us = now_us();
    return 1;
}

int nm_sky_enqueue(uint16_t heading, uint16_t scroll_y) { (void)heading; (void)scroll_y; return 1; }

static int sky_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char header[32];
    int ok = fread(header, 1, 32, f) == 32 && nm_sky_header_valid(header, 32);
    if (ok) {
        G.sky_fill[0] = nm_sky_word(header + 8); G.sky_fill[1] = nm_sky_word(header + 12);
        G.sky_rows = nm_sky_header_rows(header);
        free(G.sky_pages);
        G.sky_pages = BIG_ALLOC(4u * NM_SKY_TEXTURE_BYTES);
        ok = G.sky_pages && fread(G.sky_pages, 1, 4u * NM_SKY_TEXTURE_BYTES, f) == 4u * NM_SKY_TEXTURE_BYTES;
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ load */

static void pack_unload_split(nm_pack_t *pack);
static void world_free(nm_practice_scene_t *scene)
{
    materials_close(&G.world);
    free(G.world_binding); G.world_binding = NULL;
    free(G.world_skip); G.world_skip = NULL;
    free(G.face_lod); G.face_lod = NULL; free(G.face_level); G.face_level = NULL;
    for (unsigned l = 0; l < 2; ++l) { free(g_list[l].lod_level); g_list[l].lod_level = NULL; }
    free(scene->world_instances); scene->world_instances = NULL;
    scene->world_sources = NULL;
    if (scene->world.data) {
        pack_unload_split(&scene->world);
    }
    memset(&scene->world, 0, sizeof scene->world);
    scene->world_header = (nm_header_t){0};
    scene->world_object_first = scene->world_object_count = 0;
}

/* The world and car packs read table by table (meshpack.h's layout, the walk of meshpack.c's validate): each
   table the console draws from in its own block, so that no pack needs one block of its whole size (Expert's
   world is 2.7 MB, and PSRAM fragments over course changes); the tables only the Dreamcast reads (position
   references, strips) are skipped. pack->data is the header, the list of the other blocks and the
   vertices in one block: the Dreamcast's scene code keeps pointers into the vertices (the player's windows) and
   binds again only when pack->data moves, as with one block per pack. */
#define SPLIT_TABLES 7
static int pack_load_split(nm_pack_t *pack, const char *path)
{
#ifdef SW3D_ORIG_PACK_LOAD   /* check: the Dreamcast loader, one block (the pictures must not change) */
    return nm_pack_load(pack, path);
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    nm_header_t h;
    void **blocks = NULL;
    uint8_t *head = NULL;
    int ok = 0;
    if (fread(&h, 1, sizeof h, f) != sizeof h) goto done;
    const uint32_t magic = h.magic & ~NM_PACK_ALIGNED;
    const size_t align = h.magic & NM_PACK_ALIGNED ? 32u : 1u;
    const int layered = magic == NM_PACK_MAGIC_LAYERED;
    const int stripped = magic == NM_PACK_MAGIC_STRIPS || (layered && h.padding[0]);
    const int normal_grouped = magic == NM_PACK_MAGIC_NORMAL_GROUPED || magic == NM_PACK_MAGIC_STRIPS || (layered && h.reserved);
    const int grouped = magic == NM_PACK_MAGIC_GROUPED || normal_grouped;
    const unsigned lod_count = layered && (h.padding[3] & NM_PACK_LOD_GROUPS) ? h.padding[3] >> NM_PACK_LOD_COUNT_SHIFT : 0;
    if (magic != NM_PACK_MAGIC && magic != NM_PACK_MAGIC_SHARED && !grouped && !layered) goto done;
    const size_t lead = (sizeof h + sizeof(void *) * SPLIT_TABLES + 31u) & ~(size_t)31u;
    head = BIG_ALLOC(lead + (size_t)h.vertices * sizeof(nm_vertex_t));
    if (!head) goto done;
    memcpy(head, &h, sizeof h);
    blocks = (void **)(head + sizeof h);
    memset(blocks, 0, sizeof(void *) * SPLIT_TABLES);
    size_t offset = sizeof h;
#define ALIGN_TO(a) offset = (offset + (a) - 1) & ~(size_t)((a) - 1)
#define READ_TABLE(k, field, count, type) do { ALIGN_TO(align); const size_t bytes_ = (size_t)(count) * sizeof(type); \
        if (!(count)) goto done; blocks[k] = BIG_ALLOC(bytes_); \
        if (!blocks[k] || fseek(f, (long)offset, SEEK_SET) || fread(blocks[k], 1, bytes_, f) != bytes_) goto done; \
        pack->field = (const type *)blocks[k]; offset += bytes_; } while (0)
#define SKIP_TABLE(count, size) do { ALIGN_TO(align); offset += (size_t)(count) * (size); } while (0)
    READ_TABLE(0, meshes, h.meshes, nm_mesh_t);
    ALIGN_TO(align);
    if (!h.vertices || fseek(f, (long)offset, SEEK_SET) ||
        fread(head + lead, sizeof(nm_vertex_t), h.vertices, f) != h.vertices) goto done;
    pack->vertices = (const nm_vertex_t *)(head + lead);
    offset += (size_t)h.vertices * sizeof(nm_vertex_t);
    READ_TABLE(2, faces, h.faces, nm_face_t);
    READ_TABLE(3, materials, h.materials, nm_material_t);
    READ_TABLE(4, instances, h.instances, nm_instance_t);
    if (magic == NM_PACK_MAGIC_SHARED || grouped || layered) SKIP_TABLE(h.vertices, sizeof(uint16_t));
    if (grouped || layered) ALIGN_TO(4);
    if (grouped) {   /* the groups' bounds: sw3d culls by them */
        if (normal_grouped) READ_TABLE(6, normal_groups, h.reserved, nm_normal_group_t);
        else READ_TABLE(6, groups, h.reserved, nm_face_group_t);
    }
    if (stripped) { ALIGN_TO(32); SKIP_TABLE(h.padding[0], sizeof(nm_strip_t)); SKIP_TABLE(h.padding[1], sizeof(uint16_t)); }
    if (lod_count) { ALIGN_TO(4); READ_TABLE(5, lod_groups, lod_count, nm_lod_group_t); }
#undef READ_TABLE
#undef SKIP_TABLE
#undef ALIGN_TO
    pack->data = head;
    pack->header = (const nm_header_t *)head;
    ok = 1;
done:
    fclose(f);
    if (!ok) {
        if (blocks) for (unsigned k = 0; k < SPLIT_TABLES; ++k) free(blocks[k]);
        free(head);
        memset(pack, 0, sizeof *pack);
        printf("sw3d: cannot read %s table by table\n", path);
    }
    return ok;
}

static void pack_unload_split(nm_pack_t *pack)
{
#ifdef SW3D_ORIG_PACK_LOAD
    nm_pack_unload(pack); return;
#endif
    if (!pack->data) return;
    void **blocks = (void **)((uint8_t *)pack->data + sizeof(nm_header_t));
    for (unsigned k = 0; k < SPLIT_TABLES; ++k) free(blocks[k]);
    free(pack->data);
    memset(pack, 0, sizeof *pack);
}

void nm_practice_render_unload(nm_practice_scene_t *scene)
{
#if PIPE
    pipe_wait();
#endif
    world_free(scene);
    materials_close(&G.car);
    if (scene->model.data) {
        pack_unload_split(&scene->model);
    }
    memset(&scene->model, 0, sizeof scene->model);
    scene->car_sources = NULL;
    nm_damage_bodies_unload(&scene->damage_bodies);
    nm_damage_bodies_unload(&scene->hood_variants);
}

void nm_practice_render_release(void) {}

int nm_practice_render_load(nm_practice_scene_t *scene, const char *root, const char **stage)
{
    char file[256];
    tables_init();
    G.scene = scene;
#ifdef DAYTONA_UI
    {   /* the race's 2D actors and labels (the Dreamcast's practice_assets.c loads them with the scene and keeps
           them to the end; its loaders refuse a second load): once, before the first course's data, so that
           they never sit between the blocks of a course (a later course needs those freed as one) */
        static int ui_loaded;
        if (!ui_loaded) {
            *stage = "finish, result PAL4, result renderer, result labels";
            if (!nm_finish_render_load(root) || !nm_result_materials_load(root) || !nm_result_render_load(root, scene->math) ||
                !nm_result_labels_load(root)) return 0;
            ui_loaded = 1;
        }
    }
#endif
#if defined(ESP_PLATFORM) && !defined(SW3D_DEPTH_PSRAM)
    if (!G.depth) G.depth = heap_caps_malloc(sizeof(uint16_t) * W * H, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (!G.depth) G.depth = BIG_ALLOC(sizeof(uint16_t) * W * H);
#ifdef ESP_PLATFORM
    { static int told; if (!told) { told = 1; printf("sw3d: depth buffer in %s\n", esp_ptr_internal(G.depth) ? "internal RAM" : "PSRAM (slow)"); } }
#endif
    if (!g_pool[0].more) {   /* as many paints in internal RAM as its largest block allows (keeping 4 KB), shared by the sets */
        unsigned n = PAINT_FAST_MAX;
#ifdef ESP_PLATFORM
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest < 4096u + 128u * sizeof(paint_t)) n = 0;
        else if ((largest - 4096u) / sizeof(paint_t) < n) n = (unsigned)((largest - 4096u) / sizeof(paint_t));
#endif
        n /= SETS;
        for (unsigned st = 0; st < SETS; ++st) {
            pool_t *pl = g_pool + st;
#ifdef ESP_PLATFORM
            if (n) pl->fast = heap_caps_malloc(sizeof(paint_t) * n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
            pl->fast = malloc(sizeof(paint_t) * n);
#endif
            pl->nfast = pl->fast ? n : 0;
            pl->more = BIG_ALLOC(sizeof(paint_t) * (PAINT_CAP - pl->nfast));
            printf("sw3d: set %u: %u paints in internal RAM, %u in PSRAM\n", st, pl->nfast, PAINT_CAP - pl->nfast);
            if (!pl->more) return 0;
        }
    }
    for (unsigned st = 0; st < SETS; ++st) {
        if (!g_tris[st][0]) g_tris[st][0] = BIG_ALLOC(sizeof(tri_t) * TRI_SET_CAP);
        if (!g_tris[st][0]) { printf("sw3d: no memory for triangle lists\n"); return 0; }
#if !PIPE
        g_tris[st][1] = g_tris[st][0] + TRI_CAP;
#endif
    }
    g_list[0].tris = g_tris[0][0]; g_list[1].tris = g_tris[0][1];
#if !PIPE
    g_list[0].keys = g_keys; g_list[1].keys = g_keys + TRI_CAP; g_list[0].cap = g_list[1].cap = TRI_CAP;
#endif

    nm_practice_render_unload(scene);
    *stage = "car.dnm";
    snprintf(file, sizeof file, "%scar.dnm", root);
    if (!pack_load_split(&scene->model, file)) return 0;
    const unsigned parts = scene->model.header->instances;
    if (parts != sizeof(nm_intro_model_ids) / sizeof(nm_intro_model_ids[0])) return 0;
    for (unsigned w = 0; w < 2; ++w) {
        const unsigned binding = nm_intro_window_models[w];
        const nm_mesh_t *window = scene->model.meshes + scene->model.instances[binding].mesh;
        scene->intro_windows[w] = (nm_vertex_t *)scene->model.vertices + window->vertex_first;
        scene->intro_window_count[w] = window->vertex_count;
        for (unsigned v = 0; v < window->vertex_count && v < 36; ++v) {
            scene->intro_window_uv[w][2 * v] = scene->intro_windows[w][v].u;
            scene->intro_window_uv[w][2 * v + 1] = scene->intro_windows[w][v].v;
        }
    }
    *stage = "car-damage.dnb";
    snprintf(file, sizeof file, "%scar-damage.dnb", root);
    if (!nm_damage_bodies_load(&scene->damage_bodies, file)) return 0;
    snprintf(file, sizeof file, "%scar-hoods.dnb", root);
    if (!nm_damage_hoods_load(&scene->hood_variants, file)) return 0;
    scene->player_body_variant = NM_DAMAGE_BODY_CLEAN;
    scene->player_hood_variant = NM_DAMAGE_HOOD_CLEAN;
    if (!nm_practice_render_pose(scene)) return 0;
    scene->instance = (nm_instance_t){ .source_address = 0x95ff9d, .focus = { 310, 320 }, .viewport = { 0, 0, 495, 384 },
                                       .center = { 248, 208 } };
    scene->window_materials[0] = scene->window_materials[1] = UINT16_MAX;
    for (unsigned i = 0; i < scene->model.header->instances; ++i) {
        const uint32_t source = scene->model.instances[i].source_address;
        const unsigned kind = source == 0x1c8 || source == 0x241 ? 0u : source == 0x13b ? 1u : 2u;
        const nm_mesh_t *mesh = scene->model.meshes + scene->model.instances[i].mesh;
        if (kind < 2 && mesh->face_count) scene->window_materials[kind] = scene->model.faces[mesh->face_first].material;
    }
    scene->car_sources = scene->model.instances;
    scene->model.instances = scene->car_instances;
    scene->intro_header = *scene->model.header; scene->model.header = &scene->intro_header;
    *stage = "car.dnt";
    snprintf(file, sizeof file, "%scar.dnt", root);
    if (!nm_practice_render_lighting(scene) || !nm_practice_render_parts(scene) || !materials_open(&G.car, file)) return 0;
    free(g_car_binding);
    g_car_binding = BIG_ALLOC(sizeof(uint32_t) * scene->intro_header.faces);
    /* empty: an entry is the binding of the face's last lookup (a reused block held old ones: no car drawn) */
    if (g_car_binding) memset(g_car_binding, 0, sizeof(uint32_t) * scene->intro_header.faces);
    if (g_car_binding) memset(g_car_binding, 0, sizeof(uint32_t) * scene->intro_header.faces);

    *stage = "world.dnm";
    if (!nm_course_asset(file, sizeof file, root, scene->course, NM_COURSE_WORLD, 0)) return 0;
    if (!pack_load_split(&scene->world, file)) { printf("sw3d: no world %s\n", file); return 0; }
    if (scene->scenery.slot != scene->course || scene->scenery.instances > scene->world.header->instances) return 0;
    scene->world_header = *scene->world.header;
    scene->world_sources = scene->world.instances;
    scene->world_object_first = scene->scenery.instances;
    scene->world_object_count = scene->world.header->instances - scene->scenery.instances;
    scene->world_instances = calloc(scene->world.header->instances, sizeof *scene->world_instances);
    G.world_binding = calloc(2u * scene->world.header->faces, sizeof *G.world_binding);
    if (!scene->world_instances || !G.world_binding) return 0;
    /* face groups: a per-group visibility byte, tested once per instance */
    for (unsigned l = 0; l < 2; ++l) { free(g_list[l].group_seen); g_list[l].group_seen = NULL; }
    g_group_count = 0;
    if (nm_pack_has_groups(&scene->world)) {
        unsigned groups = 0;
        for (unsigned f = 0; f < scene->world_header.faces; ++f)
            if (scene->world.faces[f].group + 1u > groups) groups = scene->world.faces[f].group + 1u;
        for (unsigned l = 0; l < 2; ++l) g_list[l].group_seen = calloc(groups ? groups : 1, 1);
        g_group_count = g_list[0].group_seen && g_list[1].group_seen ? groups : 0;
    }
    /* level-of-detail groups (meshpack.h): which group and level each face belongs to */
    if (scene->world.lod_groups) {
        const unsigned groups = nm_pack_lod_count(&scene->world);
        G.face_lod = calloc(scene->world_header.faces, sizeof *G.face_lod);
        G.face_level = calloc(scene->world_header.faces, 1);
        for (unsigned l = 0; l < 2; ++l) g_list[l].lod_level = malloc(groups ? groups : 1);
        if (!G.face_lod || !G.face_level || !g_list[0].lod_level || !g_list[1].lod_level) return 0;
        G.lod_count = groups;
        for (unsigned g = 0; g < groups; ++g) {
            const nm_lod_group_t *lod = scene->world.lod_groups + g;
            const int sweep = lod->switch_depth[0] < 0.0f;   /* the stipple fence's sweep (stage 4): not drawn */
            unsigned f = lod->face_first;
            for (unsigned l = 0; l < lod->levels; ++l)
                for (unsigned k = 0; k < lod->face_count[l]; ++k, ++f) {
                    G.face_lod[f] = (uint16_t)(g + 1u);
                    G.face_level[f] = sweep ? 0xff : (uint8_t)l;
                }
        }
    }
    scene->world.header = &scene->world_header; scene->world.instances = scene->world_instances;
    *stage = "scenery placement";
    if (!nm_practice_render_scenery(scene)) return 0;
    *stage = "sky";
    if (nm_course_asset(file, sizeof file, root, scene->course, NM_COURSE_SKY, 0) && !sky_load(file))
        printf("sw3d: no sky %s (flat colours)\n", file);
    *stage = "world.dnt";
    if (!nm_course_asset(file, sizeof file, root, scene->course, NM_COURSE_WORLD_MATERIALS, 0) || !materials_open(&G.world, file))
        return 0;
    printf("sw3d: course %u loaded: world %u faces, %u instances; car %u faces\n", scene->course,
           scene->world_header.faces, scene->world_header.instances, scene->intro_header.faces);
#ifdef ESP_PLATFORM
    printf("sw3d: internal RAM free %u (largest %u), PSRAM free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    return 1;
}

/* ------------------------------------------------------------------ measurement */
#if defined(SW3D_BENCH) && defined(ESP_PLATFORM)
/* The span kernel alone, on a 256x256 PAL4 texture in internal RAM and in PSRAM, depth test passing and
   failing: cycles per pixel, printed once at load. */
void sw3d_kernel_bench(void)
{
    const size_t bytes = 256 * 256 / 2;
    uint8_t *tex_int = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL), *tex_ps = BIG_ALLOC(bytes);
    uint16_t *row = heap_caps_malloc(512 * 2, MALLOC_CAP_INTERNAL), *zrow = heap_caps_malloc(512 * 2, MALLOC_CAP_INTERNAL);
    uint16_t pal[16];
    if (!tex_int || !tex_ps || !row || !zrow) { printf("sw3d bench: no memory\n"); return; }
    for (size_t i = 0; i < bytes; ++i) tex_int[i] = tex_ps[i] = (uint8_t)(i * 37u);
    for (int i = 0; i < 16; ++i) pal[i] = (uint16_t)(i * 4097u);
    const int32_t du = (int32_t)(0.71f * 65536), dv = (int32_t)(0.37f * 65536);
    for (int pass = 0; pass < 4; ++pass) {
        const uint8_t *tex = pass & 1 ? tex_ps : tex_int;
        const uint16_t zfill = pass & 2 ? 0xffffu : 0u;   /* 0xffff: every pixel hidden */
        uint32_t cycles = 0;
        for (int it = 0; it < 64; ++it) {
            for (int i = 0; i < 256; ++i) zrow[i] = zfill;
            const int32_t u0 = it * 9301 * 65536 / 64, v0 = it * 4093 * 65536 / 16;
            const uint32_t t0 = CYCLES();
            span_textured(row, zrow, 256, u0, v0, du, dv, 0x40000000u, 256, 0, tex, 8, 255, 255, pal, 0xffff, 0);
            cycles += CYCLES() - t0;
        }
        printf("sw3d bench: texture %s, pixels %s: %.1f cycles a pixel\n", pass & 1 ? "PSRAM" : "internal",
               pass & 2 ? "hidden" : "drawn", cycles / (64.0 * 256.0));
    }
    free(tex_int); free(tex_ps); free(row); free(zrow);
}
#endif

/* ------------------------------------------------------------------ the Dreamcast renderer's other calls */

float nm_render_road_depth(float scale) { const float old = G.road_depth; G.road_depth = scale; return old; }
int nm_render_car_material_header(unsigned material, const uint16_t header[4]) { (void)material; (void)header; return 1; }
int nm_render_car_mesh_refresh(unsigned mesh) { (void)mesh; return 1; }
void nm_render_inspect_set_strips(int enabled) { (void)enabled; }
int nm_render_inspect_strips(void) { return 0; }
int nm_render_uv_dynamic(const nm_vertex_t *vertices, unsigned count) { (void)vertices; (void)count; return 1; }
void nm_render_world_clock_frame(unsigned group, unsigned frame) { (void)group; (void)frame; }
void nm_render_world_clock_light(unsigned group, const float model[12], const float placed[12], const float view[12])
{ (void)group; (void)model; (void)placed; (void)view; }
#ifndef DAYTONA_UI   /* the game build has the real ones (render/materials/palette_ram.c, render/ui/) */
void nm_palette_ram_defer(void) {}
int nm_palette_ram_flush(void) { return 1; }
int nm_hud_render_enqueue(int flat) { (void)flat; return 1; }
#endif
int nm_slot_glass_enqueue(const nm_header_t *world, const nm_instance_t *const placed[NM_OBJECT_SLOTS], unsigned count)
{ (void)world; (void)placed; (void)count; return 1; }
#ifndef DAYTONA_UI
int nm_finish_render_enqueue(const nm_finish_actor_t *actor, int branch, const int16_t center[2])
{ (void)actor; (void)branch; (void)center; return 1; }
#endif
/* The Dreamcast inspector camera (pad chord); not reachable on the console. */
void nm_inspect_camera_reset(nm_inspect_camera_t *camera, const float view[12]) { (void)camera; (void)view; }
void nm_inspect_camera_step(nm_inspect_camera_t *camera, float dt, float yaw, float pitch, shz_vec3_t movement, float view[12])
{ (void)camera; (void)dt; (void)yaw; (void)pitch; (void)movement; (void)view; }
shz_vec3_t nm_inspect_camera_position(const float view[12]) { (void)view; return (shz_vec3_t){ 0, 0, 0 }; }
