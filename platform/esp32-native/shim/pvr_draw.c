/* Draws one frame's captured TA records (shim/pvr_lite.c) the way the PVR would, in software, over the picture:
   polygon strips and sprites; 4/8-bit palette, ARGB1555, RGB565 and ARGB4444 textures (twiddled or not, point
   sampled); the texture environments; the depth compare and write of each header; the punch-through list's
   alpha test and the translucent list's blending. The 640x480 screen is scaled to the view. Point sampling and
   nearest filtering everywhere (the podium's bilinear photo too). */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <enDjinn/enj_enDjinn.h>
#include "pvr_lite_frame.h"

typedef struct { float x, y, z, u, v; uint32_t argb; } vtx_t;

typedef struct {
    uint32_t cmd, mode1, mode2, mode3, base;   /* base: a sprite's colour */
    int list;
    const uint8_t *texels;
    const uint32_t *palette;
    int w, h, fmt, twiddled, vq;
} state_t;

static uint16_t *g_px, *g_z;
unsigned pvr_draw_stats[6];   /* + cycles in span_simple, its pixels */
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#define DRAW_CYCLES() esp_cpu_get_cycle_count()
#else
#define DRAW_CYCLES() 0u
#endif   /* triangles, rows, fast-path pixels, other pixels (since the last read) */
static int g_w, g_h;
static float g_sx, g_sy;

/* the depth buffer's code of a 1/w: 11 mantissa bits over 2^-16 .. 2^16, positive and ordered */
static inline uint32_t depth_code(float z)
{
    union { float f; uint32_t u; } b = { z };
    if (!(z > 0.0f)) return 0;
    const int32_t c = (int32_t)(b.u >> 12) - (int32_t)((127u - 16u) << 11);
    return c < 1 ? 1u : c > 65535 ? 65535u : (uint32_t)c;
}

static inline int depth_pass(int mode, uint32_t z, uint32_t old)
{
    switch (mode) {
    case PVR_DEPTHCMP_NEVER: return 0;
    case PVR_DEPTHCMP_LESS: return z < old;
    case PVR_DEPTHCMP_EQUAL: return z == old;
    case PVR_DEPTHCMP_LEQUAL: return z <= old;
    case PVR_DEPTHCMP_GREATER: return z > old;
    case PVR_DEPTHCMP_NOTEQUAL: return z != old;
    case PVR_DEPTHCMP_GEQUAL: return z >= old;
    default: return 1;
    }
}

/* twiddled index of (x, y) in a w x h texture (the smaller side makes square blocks) */
static uint16_t interleave_lo[256];
static void interleave_init(void)
{
    static int done;
    if (done) return;
    for (unsigned v = 0; v < 256; ++v) {
        unsigned r = 0;
        for (unsigned b = 0; b < 8; ++b) r |= ((v >> b) & 1u) << (2 * b);
        interleave_lo[v] = (uint16_t)r;
    }
    done = 1;
}
static inline uint32_t spread(unsigned v) { return interleave_lo[v & 255u] | (uint32_t)interleave_lo[(v >> 8) & 3u] << 16; }
static inline uint32_t twiddle(int x, int y, int w, int h)
{
    const int side = w < h ? w : h, shift = __builtin_ctz((unsigned)side);
    const uint32_t block = (uint32_t)((x >> shift) + (y >> shift)) << (2 * shift);
    const int bx = x & (side - 1), by = y & (side - 1);
    return block | spread((unsigned)by) | spread((unsigned)bx) << 1;
}

static inline uint32_t argb1555(uint32_t p)
{
    const uint32_t r = (p >> 10) & 31u, g = (p >> 5) & 31u, b = p & 31u;
    return (p & 0x8000u ? 0xff000000u : 0u) | (r << 19 | r << 14) & 0xff0000u | (g << 11 | g << 6) & 0xff00u | (b << 3 | b >> 2);
}
static inline uint32_t rgb565_argb(uint32_t p)
{
    const uint32_t r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
    return 0xff000000u | (r << 19 | r << 14) & 0xff0000u | (g << 10 | g << 4) & 0xff00u | (b << 3 | b >> 2);
}
static inline uint32_t argb4444(uint32_t p)
{
    return ((p >> 12) & 15u) * 0x11000000u | ((p >> 8) & 15u) * 0x110000u | ((p >> 4) & 15u) * 0x1100u | (p & 15u) * 0x11u;
}

/* VQ: the codebook's 256 entries of 2x2 texels (8 bytes, in twiddled order) at the address, then one index byte
   per 2x2 block, twiddled at half the size */
static uint32_t vq_texel(const state_t *s, int x, int y)
{
    const uint8_t k = s->texels[2048u + twiddle(x >> 1, y >> 1, s->w >> 1, s->h >> 1)];
    const uint16_t t = ((const uint16_t *)(s->texels + 8u * k))[((x & 1) << 1) | (y & 1)];
    switch (s->fmt) {
    case 0: return argb1555(t);
    case 1: return rgb565_argb(t);
    case 2: return argb4444(t);
    default: return 0xffff00ffu;
    }
}

static uint32_t texel(const state_t *s, int x, int y)
{
    if (s->vq) return vq_texel(s, x, y);
    const uint32_t i = s->twiddled ? twiddle(x, y, s->w, s->h) : (uint32_t)(y * s->w + x);
    switch (s->fmt) {
    case 5: { const uint8_t b = s->texels[i >> 1]; return s->palette[(i & 1 ? b >> 4 : b & 15u)]; }
    case 6: return s->palette[s->texels[i]];
    case 0: return argb1555(((const uint16_t *)s->texels)[i]);
    case 1: return rgb565_argb(((const uint16_t *)s->texels)[i]);
    case 2: return argb4444(((const uint16_t *)s->texels)[i]);
    default: return 0xffff00ffu;
    }
}

static inline int ifloor(float v) { const int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline int iceil(float v) { const int i = (int)v; return (float)i < v ? i + 1 : i; }

static inline uint32_t mul8(uint32_t a, uint32_t b) { const uint32_t t = a * b + 128u; return (t + (t >> 8)) >> 8; }

static inline uint32_t blend_factor(int f, uint32_t src, uint32_t dst, int ch)   /* ch: 24 alpha, 16/8/0 colour */
{
    switch (f) {
    case PVR_BLEND_ZERO: return 0;
    case PVR_BLEND_ONE: return 255;
    case PVR_BLEND_DESTCOLOR: return (dst >> ch) & 255u;
    case PVR_BLEND_INVDESTCOLOR: return 255u - ((dst >> ch) & 255u);
    case PVR_BLEND_SRCALPHA: return src >> 24;
    case PVR_BLEND_INVSRCALPHA: return 255u - (src >> 24);
    case PVR_BLEND_DESTALPHA: return 255;
    default: return 0;
    }
}

static inline uint32_t from565(uint16_t p) { return rgb565_argb(p); }
static inline uint16_t to565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu)); }

/* one pixel: texture, environment, list rule, depth */
static inline void shade(const state_t *s, int px, int py, float z, float u, float v, uint32_t col)
{
    const int mode = (int)PVR_LITE_GET(s->mode1, 29, 3), zwrite = !PVR_LITE_GET(s->mode1, 26, 1);
    const uint32_t zc = depth_code(z);
    uint16_t *zp = g_z + py * g_w + px;
    if (!depth_pass(mode, zc, *zp)) return;
    uint32_t c = col;
    if (s->texels) {
        int tx = ifloor(u * (float)s->w), ty = ifloor(v * (float)s->h);
        const uint32_t clamp = PVR_LITE_GET(s->mode2, 15, 2), flip = PVR_LITE_GET(s->mode2, 17, 2);
        if (clamp & 2) tx = tx < 0 ? 0 : tx >= s->w ? s->w - 1 : tx;
        else if ((flip & 2) && (tx / s->w + (tx < 0)) & 1) tx = s->w - 1 - (tx & (s->w - 1));
        if (clamp & 1) ty = ty < 0 ? 0 : ty >= s->h ? s->h - 1 : ty;
        else if ((flip & 1) && (ty / s->h + (ty < 0)) & 1) ty = s->h - 1 - (ty & (s->h - 1));
        tx &= s->w - 1; ty &= s->h - 1;
        uint32_t t = texel(s, tx, ty);
        if (PVR_LITE_GET(s->mode2, 19, 1)) t |= 0xff000000u;   /* texture alpha ignored */
        const uint32_t ta = t >> 24, ca = col >> 24;
        switch (PVR_LITE_GET(s->mode2, 6, 2)) {
        case PVR_TXRENV_REPLACE: c = t; break;
        case PVR_TXRENV_MODULATE:
            c = ta << 24 | mul8((t >> 16) & 255u, (col >> 16) & 255u) << 16 | mul8((t >> 8) & 255u, (col >> 8) & 255u) << 8 | mul8(t & 255u, col & 255u);
            break;
        case PVR_TXRENV_DECAL: {
            uint32_t r = 0;
            for (int ch = 0; ch <= 16; ch += 8)
                r |= (mul8((t >> ch) & 255u, ta) + mul8((col >> ch) & 255u, 255u - ta)) << ch;
            c = ca << 24 | r;
            break;
        }
        default:
            c = mul8(ta, ca) << 24 | mul8((t >> 16) & 255u, (col >> 16) & 255u) << 16 | mul8((t >> 8) & 255u, (col >> 8) & 255u) << 8 | mul8(t & 255u, col & 255u);
            break;
        }
    }
    if (!PVR_LITE_GET(s->mode2, 20, 1)) c |= 0xff000000u;   /* alpha not used */
    uint16_t *pp = g_px + py * g_w + px;
    if (s->list == PVR_LIST_PT_POLY && (c >> 24) < 128u) return;
    if (s->list == PVR_LIST_TR_POLY) {
        const uint32_t d = from565(*pp);
        const int sf = (int)PVR_LITE_GET(s->mode2, 29, 3), df = (int)PVR_LITE_GET(s->mode2, 26, 3);
        uint32_t r = 0;
        for (int ch = 0; ch <= 16; ch += 8) {
            uint32_t v8 = mul8((c >> ch) & 255u, blend_factor(sf, c, d, ch)) + mul8((d >> ch) & 255u, blend_factor(df, c, d, ch));
            r |= (v8 > 255u ? 255u : v8) << ch;
        }
        c = r;
    }
    *pp = to565(c);
    if (zwrite) *zp = (uint16_t)zc;
}

/* The common span of the 2D screens: a twiddled 4-bit palette (fmt 5) or ARGB1555 (fmt 0) texture, uncoloured
   (white), the opaque or punch-through list, a constant depth code; v constant along the span when dv is 0
   (screen-aligned quads), so its twiddle bits are worked out once. */
static inline __attribute__((always_inline)) void span_simple(uint16_t *pp, uint16_t *zp, int n, int32_t U, int32_t V,
        int32_t dU, int32_t dV, const uint8_t *texels, const uint32_t *palette, int wm, int hm, int shift, int side,
        uint32_t code, const int fmt, const int pt, const int test, const int zwrite)
{
    const uint32_t smask = (uint32_t)side - 1u;
    uint32_t ybits = 0, yblock = 0;
    if (!dV) { const uint32_t ty = (uint32_t)(V >> 16) & (uint32_t)hm; ybits = spread(ty & smask); yblock = ty >> shift; }
    for (; n; --n, ++pp, ++zp, U += dU, V += dV) {
        if (test && code < *zp) continue;
        if (dV) { const uint32_t ty = (uint32_t)(V >> 16) & (uint32_t)hm; ybits = spread(ty & smask); yblock = ty >> shift; }
        const uint32_t tx = (uint32_t)(U >> 16) & (uint32_t)wm;
        const uint32_t i = (((tx >> shift) + yblock) << (2 * shift)) | ybits | spread(tx & smask) << 1;
        uint32_t c;
        if (fmt == 5) {
            const uint8_t b = texels[i >> 1];
            c = palette[i & 1 ? b >> 4 : b & 15u];
            if (pt && (c >> 24) < 128u) continue;
            *pp = to565(c);
        } else {
            const uint16_t t = ((const uint16_t *)texels)[i];
            if (pt && !(t & 0x8000u)) continue;
            *pp = (uint16_t)(((t & 0x7fe0u) << 1) | ((t & 0x0200u) >> 4) | (t & 0x1fu));   /* 1555 -> 565 */
        }
        if (zwrite) *zp = (uint16_t)code;
    }
}

/* A triangle in screen pixels, by rows: each row's span from the edges, the attributes as planes stepped along
   it. u and v are perspective-correct (z is 1/w) unless z is the same at the three corners (the 2D screens'
   quads), where they step linearly in 16.16 and the depth code is worked out once; that case, with a 4-bit
   palette texture, a white or flat colour and the opaque or punch-through list, has a loop of its own. */
static void triangle(const state_t *s, vtx_t a, vtx_t b, vtx_t c)
{
    const float den = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (fabsf(den) < 1e-6f) return;
    const float id = 1.0f / den;
#define GRAD(A, B, C, dx, dy) const float dx = (((B) - (A)) * (c.y - a.y) - ((C) - (A)) * (b.y - a.y)) * id, \
                                          dy = (((C) - (A)) * (b.x - a.x) - ((B) - (A)) * (c.x - a.x)) * id
    GRAD(a.z, b.z, c.z, dzdx, dzdy);
    const int flat_z = a.z == b.z && b.z == c.z;
    const int textured = s->texels != NULL;
    const int gouraud = PVR_LITE_GET(s->cmd, 1, 1) && (s->cmd >> 29) != 5u && !(a.argb == b.argb && b.argb == c.argb);
    /* u, v as planes of u*z, v*z (or of u, v when z is flat) */
    const float ua = flat_z ? a.u : a.u * a.z, ub = flat_z ? b.u : b.u * b.z, uc = flat_z ? c.u : c.u * c.z;
    const float va = flat_z ? a.v : a.v * a.z, vb = flat_z ? b.v : b.v * b.z, vc = flat_z ? c.v : c.v * c.z;
    GRAD(ua, ub, uc, dudx, dudy);
    GRAD(va, vb, vc, dvdx, dvdy);
    float cdx[4] = { 0 }, cdy[4] = { 0 }, c0[4] = { 0 };
    if (gouraud)
        for (int k = 0; k < 4; ++k) {
            const float A = (float)((a.argb >> (8 * k)) & 255u), B = (float)((b.argb >> (8 * k)) & 255u), C = (float)((c.argb >> (8 * k)) & 255u);
            GRAD(A, B, C, gx, gy);
            cdx[k] = gx; cdy[k] = gy; c0[k] = A;
        }
#undef GRAD
    const float miny = fminf(a.y, fminf(b.y, c.y)), maxy = fmaxf(a.y, fmaxf(b.y, c.y));
    int y0 = iceil(miny - 0.5f), y1 = iceil(maxy - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    /* edges: top -> bottom (long), top -> middle, middle -> bottom; x stepped down them */
    const vtx_t *top = &a, *mid = &b, *bot = &c, *sw;
    if (mid->y < top->y) { sw = top; top = mid; mid = sw; }
    if (bot->y < top->y) { sw = top; top = bot; bot = sw; }
    if (bot->y < mid->y) { sw = mid; mid = bot; bot = sw; }
    const float long_dx = bot->y > top->y ? (bot->x - top->x) / (bot->y - top->y) : 0.0f;
    const float top_dx = mid->y > top->y ? (mid->x - top->x) / (mid->y - top->y) : 0.0f;
    const float bot_dx = bot->y > mid->y ? (bot->x - mid->x) / (bot->y - mid->y) : 0.0f;
    const uint32_t flat_code = depth_code(a.z);
    const int mode = (int)PVR_LITE_GET(s->mode1, 29, 3), zwrite = !PVR_LITE_GET(s->mode1, 26, 1);
    const uint32_t col = c.argb;
    /* the fast case: z and colour the same over the triangle (the 2D screens' quads and sprites) */
#ifdef PVR_DRAW_SLOW   /* check: every pixel through shade() */
    const int fast = 0;
#else
    const int fast = flat_z && !gouraud;
#endif
    const uint32_t env = PVR_LITE_GET(s->mode2, 6, 2), clamp = PVR_LITE_GET(s->mode2, 15, 2), flip = PVR_LITE_GET(s->mode2, 17, 2);
    const int alpha_used = PVR_LITE_GET(s->mode2, 20, 1), tex_alpha = !PVR_LITE_GET(s->mode2, 19, 1);
    const int white = (col | 0xff000000u) == 0xffffffffu;
    const uint32_t col_a = col >> 24, col_r = (col >> 16) & 255u, col_g = (col >> 8) & 255u, col_b = col & 255u;
    const int sf = (int)PVR_LITE_GET(s->mode2, 29, 3), df = (int)PVR_LITE_GET(s->mode2, 26, 3);
    const int plain_blend = sf == PVR_BLEND_SRCALPHA && df == PVR_BLEND_INVSRCALPHA;
    /* an untextured fill of one colour (opaque, or translucent with source alpha) */
    const int flat_fill = fast && !textured && s->list != PVR_LIST_PT_POLY && (s->list != PVR_LIST_TR_POLY || plain_blend);
    /* span_simple's case */
    const int simple = fast && textured && s->twiddled && !s->vq && (s->fmt == 5 || s->fmt == 0) && white && !(clamp | flip) &&
                       s->list != PVR_LIST_TR_POLY && (mode == PVR_DEPTHCMP_GEQUAL || mode == PVR_DEPTHCMP_ALWAYS) &&
                       (s->list != PVR_LIST_PT_POLY || (alpha_used && tex_alpha)) &&
                       (env == PVR_TXRENV_MODULATE || env == PVR_TXRENV_MODULATEALPHA || env == PVR_TXRENV_REPLACE) &&
                       (s->fmt == 5 || alpha_used || s->list == PVR_LIST_OP_POLY);
    for (int y = y0; y < y1; ++y) {
        const float py = (float)y + 0.5f;
        if (py < top->y || py >= bot->y) continue;
        const float xa = top->x + (py - top->y) * long_dx;
        const float xb = py < mid->y ? top->x + (py - top->y) * top_dx : mid->x + (py - mid->y) * bot_dx;
        const float xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
        int x0 = iceil(xl - 0.5f), x1 = iceil(xr - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > g_w) x1 = g_w;
        if (x0 >= x1) continue;
        const float fx = (float)x0 + 0.5f - a.x, fy = py - a.y;
        uint16_t *zp = g_z + y * g_w + x0, *pp = g_px + y * g_w + x0;
        ++pvr_draw_stats[1];
        pvr_draw_stats[fast ? 2 : 3] += (unsigned)(x1 - x0);
        if (fast) {
            const float fw = (float)(textured ? s->w : 1) * 65536.0f, fh = (float)(textured ? s->h : 1) * 65536.0f;
            int32_t U = (int32_t)((ua + dudx * fx + dudy * fy) * fw), V = (int32_t)((va + dvdx * fx + dvdy * fy) * fh);
            const int32_t dU = (int32_t)(dudx * fw), dV = (int32_t)(dvdx * fh);
            const int wm = s->w - 1, hm = s->h - 1, list = s->list;
            const int side = textured ? (s->w < s->h ? s->w : s->h) : 1, shift = __builtin_ctz((unsigned)side);
            if (flat_fill) {   /* untextured, one colour: opaque, or blended with its alpha */
                uint16_t *q = pp, *zq = zp;
                const uint32_t a8 = alpha_used ? col_a : 255u, ia = 255u - a8;
                const uint32_t sr = mul8(col_r, a8), sg = mul8(col_g, a8), sb = mul8(col_b, a8);
                const uint16_t solid = to565(col);
                for (int x = x0; x < x1; ++x, ++q, ++zq) {
                    if (!depth_pass(mode, flat_code, *zq)) continue;
                    if (s->list == PVR_LIST_TR_POLY) {
                        const uint32_t d = from565(*q);
                        *q = to565((sr + mul8((d >> 16) & 255u, ia)) << 16 | (sg + mul8((d >> 8) & 255u, ia)) << 8 | (sb + mul8(d & 255u, ia)));
                    } else *q = solid;
                    if (zwrite) *zq = (uint16_t)flat_code;
                }
                continue;
            }
            if (simple) {
                const int n = x1 - x0, ge = mode == PVR_DEPTHCMP_GEQUAL;
                const uint32_t c0_ = DRAW_CYCLES();
#if defined(PVR_DRAW_FAKE_TEX) || defined(PVR_DRAW_DUMMY_FB)
                static uint8_t fake_tex[8192]; static uint16_t dummy_row[640], dummy_z[640];   /* measurements: internal RAM */
#endif
#ifdef PVR_DRAW_FAKE_TEX
                const uint8_t *tex_ = fake_tex; const int wm_ = 127, hm_ = 127, shift_ = 7, side_ = 128;
#else
                const uint8_t *tex_ = s->texels; const int wm_ = wm, hm_ = hm, shift_ = shift, side_ = side;
#endif
#ifdef PVR_DRAW_DUMMY_FB
                uint16_t *pp_ = dummy_row, *zp_ = dummy_z;
#else
                uint16_t *pp_ = pp, *zp_ = zp;
#endif
#define SPAN(F, PT, T, ZW) span_simple(pp_, zp_, n, U, V, dU, dV, tex_, s->palette, wm_, hm_, shift_, side_, flat_code, F, PT, T, ZW)
                if (s->fmt == 5) {
                    if (s->list == PVR_LIST_PT_POLY) { if (ge) { if (zwrite) SPAN(5, 1, 1, 1); else SPAN(5, 1, 1, 0); } else { if (zwrite) SPAN(5, 1, 0, 1); else SPAN(5, 1, 0, 0); } }
                    else { if (ge) { if (zwrite) SPAN(5, 0, 1, 1); else SPAN(5, 0, 1, 0); } else { if (zwrite) SPAN(5, 0, 0, 1); else SPAN(5, 0, 0, 0); } }
                } else {
                    if (s->list == PVR_LIST_PT_POLY) { if (ge) { if (zwrite) SPAN(0, 1, 1, 1); else SPAN(0, 1, 1, 0); } else { if (zwrite) SPAN(0, 1, 0, 1); else SPAN(0, 1, 0, 0); } }
                    else { if (ge) { if (zwrite) SPAN(0, 0, 1, 1); else SPAN(0, 0, 1, 0); } else { if (zwrite) SPAN(0, 0, 0, 1); else SPAN(0, 0, 0, 0); } }
                }
#undef SPAN
                pvr_draw_stats[4] += DRAW_CYCLES() - c0_; pvr_draw_stats[5] += (unsigned)n;
                continue;
            }
            for (int x = x0; x < x1; ++x, ++zp, ++pp, U += dU, V += dV) {
                if (!depth_pass(mode, flat_code, *zp)) continue;
                uint32_t c = col;
                if (textured) {
                    int tx = U >> 16, ty = V >> 16;
                    if (clamp | flip) {
                        if (clamp & 2) tx = tx < 0 ? 0 : tx > wm ? wm : tx; else if ((flip & 2) && ((tx >> __builtin_ctz((unsigned)s->w)) & 1)) tx = wm - (tx & wm);
                        if (clamp & 1) ty = ty < 0 ? 0 : ty > hm ? hm : ty; else if ((flip & 1) && ((ty >> __builtin_ctz((unsigned)s->h)) & 1)) ty = hm - (ty & hm);
                    }
                    tx &= wm; ty &= hm;
                    uint32_t t;
                    if (s->twiddled && !s->vq) {
                        const uint32_t i = ((uint32_t)((tx >> shift) + (ty >> shift)) << (2 * shift)) |
                                           spread((unsigned)(ty & (side - 1))) | spread((unsigned)(tx & (side - 1))) << 1;
                        switch (s->fmt) {
                        case 5: { const uint8_t b = s->texels[i >> 1]; t = s->palette[i & 1 ? b >> 4 : b & 15u]; break; }
                        case 6: t = s->palette[s->texels[i]]; break;
                        case 0: t = argb1555(((const uint16_t *)s->texels)[i]); break;
                        case 1: t = rgb565_argb(((const uint16_t *)s->texels)[i]); break;
                        case 2: t = argb4444(((const uint16_t *)s->texels)[i]); break;
                        default: t = 0xffff00ffu; break;
                        }
                    } else t = texel(s, tx, ty);
                    if (!tex_alpha) t |= 0xff000000u;
                    if (env == PVR_TXRENV_REPLACE || white) c = env == PVR_TXRENV_MODULATEALPHA ? (t & 0xffffffu) | mul8(t >> 24, col_a) << 24 : env == PVR_TXRENV_DECAL ? (t & 0xffffffu) | col_a << 24 : t;
                    else {
                        const uint32_t ta = t >> 24;
                        if (env == PVR_TXRENV_DECAL) {
                            c = col_a << 24;
                            for (int ch = 0; ch <= 16; ch += 8) c |= (mul8((t >> ch) & 255u, ta) + mul8((col >> ch) & 255u, 255u - ta)) << ch;
                        } else
                            c = (env == PVR_TXRENV_MODULATEALPHA ? mul8(ta, col_a) : ta) << 24 | mul8((t >> 16) & 255u, col_r) << 16 |
                                mul8((t >> 8) & 255u, col_g) << 8 | mul8(t & 255u, col_b);
                    }
                }
                if (!alpha_used) c |= 0xff000000u;
                if (list == PVR_LIST_PT_POLY) { if ((c >> 24) < 128u) continue; }
                else if (list == PVR_LIST_TR_POLY) {
                    const uint32_t d = from565(*pp);
                    if (plain_blend) {
                        const uint32_t a8 = c >> 24, ia = 255u - a8;
                        c = (mul8((c >> 16) & 255u, a8) + mul8((d >> 16) & 255u, ia)) << 16 | (mul8((c >> 8) & 255u, a8) + mul8((d >> 8) & 255u, ia)) << 8 |
                            (mul8(c & 255u, a8) + mul8(d & 255u, ia));
                    } else {
                        uint32_t r = 0;
                        for (int ch = 0; ch <= 16; ch += 8) {
                            const uint32_t v8 = mul8((c >> ch) & 255u, blend_factor(sf, c, d, ch)) + mul8((d >> ch) & 255u, blend_factor(df, c, d, ch));
                            r |= (v8 > 255u ? 255u : v8) << ch;
                        }
                        c = r;
                    }
                }
                *pp = to565(c);
                if (zwrite) *zp = (uint16_t)flat_code;
            }
            continue;
        }
        float z = a.z + dzdx * fx + dzdy * fy, u = ua + dudx * fx + dudy * fy, v = va + dvdx * fx + dvdy * fy;
        float cc[4];
        for (int k = 0; k < 4; ++k) cc[k] = c0[k] + cdx[k] * fx + cdy[k] * fy;
        for (int x = x0; x < x1; ++x, z += dzdx, u += dudx, v += dvdx) {
            float tu = u, tv = v;
            if (textured && !flat_z && z > 0.0f) { const float iz = 1.0f / z; tu = u * iz; tv = v * iz; }
            uint32_t pc = col;
            if (gouraud) {
                pc = 0;
                for (int k = 0; k < 4; ++k) {
                    const int q = (int)(cc[k] + 0.5f);
                    pc |= (uint32_t)(q < 0 ? 0 : q > 255 ? 255 : q) << (8 * k);
                    cc[k] += cdx[k];
                }
            }
            shade(s, x, y, flat_z ? a.z : z, tu, tv, pc);
        }
    }
    (void)g_px;
}

static void header(state_t *s, const uint32_t *w)
{
    s->cmd = w[0]; s->mode1 = w[1]; s->mode2 = w[2]; s->mode3 = w[3];
    s->base = (w[0] >> 29) == 5u ? w[4] : 0xffffffffu;
    s->texels = NULL;
    if (PVR_LITE_GET(w[0], 3, 1)) {
        s->texels = pvr_lite_texture(w[3]);
        s->w = 8 << PVR_LITE_GET(w[2], 3, 3);
        s->h = 8 << PVR_LITE_GET(w[2], 0, 3);
        s->fmt = (int)PVR_LITE_GET(w[3], 27, 3);
        s->twiddled = s->fmt >= 5 || !PVR_LITE_GET(w[3], 26, 1);
        s->vq = (int)PVR_LITE_GET(w[3], 30, 1);
        s->palette = s->palette;   /* bank applied below */
    }
}

static inline float uv16(uint32_t half) { union { uint32_t u; float f; } b = { half << 16 }; return b.f; }

static volatile int draw_busy;
void pvr_lite_draw(const pvr_lite_frame_t *f, uint16_t *pixels, uint16_t *depth, int w, int h)
{
    if (__atomic_fetch_add(&draw_busy, 1, __ATOMIC_ACQ_REL)) printf("pvr_draw: two draws at once!\n");
    void pvr_lite_draw_body(const pvr_lite_frame_t *f, uint16_t *pixels, uint16_t *depth, int w, int h);
    pvr_lite_draw_body(f, pixels, depth, w, h);
    __atomic_fetch_sub(&draw_busy, 1, __ATOMIC_ACQ_REL);
}
void pvr_lite_draw_body(const pvr_lite_frame_t *f, uint16_t *pixels, uint16_t *depth, int w, int h)
{
    interleave_init();
    g_px = pixels; g_z = depth; g_w = w; g_h = h;
    g_sx = (float)w / 640.0f; g_sy = (float)h / 480.0f;
    memset(depth, 0, sizeof(uint16_t) * (size_t)w * (size_t)h);
    state_t s = { 0 };
    s.list = PVR_LIST_OP_POLY;
    vtx_t strip[2];
    int strip_n = 0, sprite_half = 0, in_sprite = 0;
    uint32_t sprite_first[8];
    for (unsigned r = 0; r < f->count; ++r) {
        const uint32_t *word = f->records + 8u * r;
        if (in_sprite && sprite_half) {   /* the second half of a sprite vertex: cy cz dx dy - auv buv cuv */
            sprite_half = 0;
            union { uint32_t u; float f; } q;
            const float *a = (const float *)(sprite_first + 1);
            float cy, cz, dx, dy;
            q.u = word[0]; cy = q.f; q.u = word[1]; cz = q.f; q.u = word[2]; dx = q.f; q.u = word[3]; dy = q.f;
            vtx_t A = { a[0], a[1], a[2], 0, 0, s.base }, B = { a[3], a[4], a[5], 0, 0, s.base }, C = { a[6], cy, cz, 0, 0, s.base };
            if (s.texels) {
                A.u = uv16(word[5] >> 16); A.v = uv16(word[5] & 0xffffu);
                B.u = uv16(word[6] >> 16); B.v = uv16(word[6] & 0xffffu);
                C.u = uv16(word[7] >> 16); C.v = uv16(word[7] & 0xffffu);
            }
            vtx_t D = { dx, dy, A.z + C.z - B.z, A.u + C.u - B.u, A.v + C.v - B.v, s.base };
            vtx_t *all[4] = { &A, &B, &C, &D };
            for (int k = 0; k < 4; ++k) { all[k]->x *= g_sx; all[k]->y *= g_sy; }
            triangle(&s, A, B, C);
            triangle(&s, A, C, D);
            continue;
        }
        const uint32_t type = word[0] >> 29;
        if (word[0] == 0) {   /* a list marker (pvr_lite_capture) */
            s.list = (int)word[1]; strip_n = 0; in_sprite = 0; s.texels = NULL;
            continue;
        }
        if (type == 4u || type == 5u) {   /* polygon or sprite header */
            header(&s, word);
            in_sprite = type == 5u;
            strip_n = 0;
            if (s.texels) {
                if (s.fmt == 5) s.palette = f->palette + (PVR_LITE_GET(s.mode3, 21, 6) << 4);
                else if (s.fmt == 6) s.palette = f->palette + (PVR_LITE_GET(s.mode3, 25, 2) << 8);
            }
            continue;
        }
        if (type != 7u) continue;   /* user clip and the rest */
        if (in_sprite) { memcpy(sprite_first, word, 32); sprite_half = 1; continue; }
        /* a strip vertex: x y z, u v (float, or both in one word with 16-bit u/v), base colour */
        union { uint32_t u; float f; } q;
        vtx_t v;
        q.u = word[1]; v.x = q.f * g_sx; q.u = word[2]; v.y = q.f * g_sy; q.u = word[3]; v.z = q.f;
        if (PVR_LITE_GET(s.cmd, 0, 1)) { v.u = uv16(word[4] >> 16); v.v = uv16(word[4] & 0xffffu); v.argb = word[6]; }
        else { q.u = word[4]; v.u = q.f; q.u = word[5]; v.v = q.f; v.argb = word[6]; }
        if (strip_n >= 2) {
            triangle(&s, strip[0], strip[1], v);
            strip[0] = strip[1]; strip[1] = v;
        } else strip[strip_n++] = v;
        if (word[0] & 0x10000000u) strip_n = 0;   /* end of strip */
    }
}
