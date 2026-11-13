/* DAYTONA_MODE=bench: what integer, floating point and memory operations cost on this console's ESP32-S3,
   in CPU cycles (240 MHz), measured on core 1 with the cycle counter. Results go to the USB serial port.
   bench_ops.S holds the instruction loops; the rest (library calls, divides, doubles, memory) is C here. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#define CYCLES() esp_cpu_get_cycle_count()

typedef void bench_fn(uint32_t iterations, void *pointer);
#define OPS(X) \
    X(nop, "nop (1 cycle reference)") \
    X(add_lat, "add, dependent") X(add_thr, "add x4, independent") X(addx2_lat, "addx2, dependent") \
    X(mull_lat, "mull 32x32, dependent") X(mull_thr, "mull x4, independent") X(mulsh_lat, "mulsh (high word)") \
    X(mul16_lat, "mul16u, dependent") \
    X(quou_thr, "quou (int divide), big operand") X(quou_lat, "quou + or, dependent") X(quos_thr, "quos (signed divide)") \
    X(remu_thr, "remu (int remainder)") X(quou_small, "quou, 300/7") \
    X(ssl_sll, "ssl + sll (variable shift)") X(ssr_srl, "ssr + srl (variable shift)") X(extui_lat, "extui, dependent") \
    X(srai_lat, "srai, dependent") X(nsau_lat, "nsau (count leading zeros)") X(clamps_lat, "clamps (saturate)") \
    X(minmax, "min + max") X(moveqz, "moveqz (select)") \
    X(br_taken, "branch taken + add") X(br_not, "branch not taken + nop + add") X(bbci_taken, "bbci taken + add") \
    X(adds_lat, "add.s, dependent") X(adds_thr, "add.s x4, independent") X(muls_lat, "mul.s, dependent") \
    X(muls_thr, "mul.s x4, independent") X(madds_lat, "madd.s, dependent") X(madds_thr, "madd.s x4, independent") \
    X(recip0_lat, "recip0.s (reciprocal seed)") X(rsqrt0_lat, "rsqrt0.s (1/sqrt seed)") X(div0_lat, "div0.s") \
    X(divn_lat, "divn.s") X(f2i_i2f, "float.s + trunc.s round trip") X(trunc_thr, "trunc.s x4 (float->fixed)") \
    X(float_thr, "float.s x4 (int->float)") X(wfr_rfr, "wfr + rfr round trip") X(olt_br, "olt.s + bt taken + add") \
    X(adds_rfr, "add.s then rfr (use in int)") X(muls_trunc, "mul.s then trunc.s")
#define MEMOPS(X) \
    X(l32i_lat, "l32i load-to-use (pointer chase)") X(l32i_thr, "l32i x4, independent") \
    X(l8_use, "l8ui + add (use)") X(l16_use, "l16ui + add (use)") X(l16_addr, "l16ui -> addx2 -> l16ui (table)") \
    X(s16_stream, "s16i + addi, streaming") X(s32_stream, "s32i + addi, streaming") \
    X(ld_st16, "l16ui + s16i + addi (read-modify)") X(lsi_use, "lsi + add.s (use)")
#define DECL(n, s) bench_fn bench_##n;
OPS(DECL) MEMOPS(DECL)

static double run(bench_fn *f, uint32_t iterations, void *p, unsigned per_body)
{
    uint32_t best = UINT32_MAX;
    for (int r = 0; r < 5; ++r) {
        const uint32_t t0 = CYCLES();
        f(iterations, p);
        const uint32_t c = CYCLES() - t0;
        if (c < best) best = c;
    }
    (void)per_body;
    return (double)best / (iterations * 16.0);
}

/* C-level costs: a loop of N calls of the operation, its result folded into a volatile sink */
static volatile float vf_sink, vf_a = 1.2345f, vf_b = 0.7654f;
static volatile double vd_sink, vd_a = 1.2345, vd_b = 0.7654;
static volatile int32_t vi_sink, vi_a = 1234567, vi_b = 77;
static volatile int64_t vl_sink, vl_a = 123456789012LL, vl_b = 7777;
__attribute__((noinline)) static int empty_call(int x) { __asm__ volatile(""); return x + 1; }

static float recip_table[256];
static inline float fast_recip(float x)
{
    union { float f; uint32_t u; } in = { x }, t, out;
    const uint32_t e = (in.u >> 23) & 255u;
    t.f = recip_table[(in.u >> 15) & 255u];
    out.u = (in.u & 0x80000000u) | ((253u - e) << 23) | (t.u & 0x7fffffu);
    return out.f * (2.0f - x * out.f);
}

/* the FPU's reciprocal seed (recip0.s) and Newton steps: an alternative to the table */
static inline float hw_recip1(float x)
{
    float r;
    __asm__("recip0.s %0, %1" : "=f"(r) : "f"(x));
    return r * (2.0f - x * r);
}
static inline float hw_recip2(float x)
{
    float r = hw_recip1(x);
    return r * (2.0f - x * r);
}
/* 1/sqrt from the bits and two Newton steps (lighting.h's EspRacer path) */
static inline float bit_rsqrt(float x)
{
    union { float f; uint32_t u; } r = { x };
    const float half = 0.5f * x;
    r.u = 0x5f3759dfu - (r.u >> 1);
    r.f *= 1.5f - half * r.f * r.f;
    r.f *= 1.5f - half * r.f * r.f;
    return r.f;
}
static inline float hw_rsqrt(float x)
{
    float r;
    __asm__("rsqrt0.s %0, %1" : "=f"(r) : "f"(x));
    const float half = 0.5f * x;
    r *= 1.5f - half * r * r;
    return r;
}

/* the largest relative error of each over a sweep of 1e-4 .. 1e4 */
static void accuracy(void)
{
    double e_tab = 0, e_hw0 = 0, e_hw1 = 0, e_hw2 = 0, e_rs = 0, e_hrs0 = 0, e_hrs = 0;
    for (int i = 0; i < 200000; ++i) {
        const float x = 1e-4f * powf(1e8f, (float)i / 200000.0f);
        const double exact = 1.0 / (double)x, rexact = 1.0 / sqrt((double)x);
        float r0; __asm__("recip0.s %0, %1" : "=f"(r0) : "f"(x));
        float s0; __asm__("rsqrt0.s %0, %1" : "=f"(s0) : "f"(x));
        const double a = fabs(fast_recip(x) * x - 1.0), b = fabs((double)r0 / exact - 1.0), c = fabs(hw_recip1(x) / exact - 1.0),
                     d = fabs(hw_recip2(x) / exact - 1.0), e = fabs(bit_rsqrt(x) / rexact - 1.0), f = fabs((double)s0 / rexact - 1.0),
                     g = fabs(hw_rsqrt(x) / rexact - 1.0);
        if (a > e_tab) e_tab = a;
        if (b > e_hw0) e_hw0 = b;
        if (c > e_hw1) e_hw1 = c;
        if (d > e_hw2) e_hw2 = d;
        if (e > e_rs) e_rs = e;
        if (f > e_hrs0) e_hrs0 = f;
        if (g > e_hrs) e_hrs = g;
    }
    printf("bench acc 1/x table + 1 Newton          %.2e\n", e_tab);
    printf("bench acc 1/x recip0.s alone            %.2e\n", e_hw0);
    printf("bench acc 1/x recip0.s + 1 Newton       %.2e\n", e_hw1);
    printf("bench acc 1/x recip0.s + 2 Newton       %.2e\n", e_hw2);
    printf("bench acc 1/sqrt bits + 2 Newton        %.2e\n", e_rs);
    printf("bench acc 1/sqrt rsqrt0.s alone         %.2e\n", e_hrs0);
    printf("bench acc 1/sqrt rsqrt0.s + 1 Newton    %.2e\n", e_hrs);
}

#define N 2000
#define TIME_C(label, body) do { \
        uint32_t best_ = UINT32_MAX; \
        for (int r_ = 0; r_ < 3; ++r_) { \
            const uint32_t t0_ = CYCLES(); \
            for (int i_ = 0; i_ < N; ++i_) { body; } \
            const uint32_t c_ = CYCLES() - t0_; if (c_ < best_) best_ = c_; } \
        printf("bench c   %-36s %8.1f\n", label, (double)best_ / N - loop_cost); } while (0)

static void c_tests(void)
{
    for (unsigned i = 0; i < 256; ++i) recip_table[i] = 1.0f / (1.0f + ((float)i + 0.5f) / 256.0f);
    double loop_cost = 0;
    {   /* the loop with a volatile load and store alone, subtracted from the rest */
        uint32_t t0 = CYCLES();
        for (int i = 0; i < N; ++i) vf_sink = vf_a;
        loop_cost = (double)(CYCLES() - t0) / N;
        printf("bench c   %-36s %8.1f  (subtracted below)\n", "loop + volatile load/store", loop_cost);
    }
    TIME_C("float a*b", vf_sink = vf_a * vf_b);
    TIME_C("float a/b (__divsf3)", vf_sink = vf_a / vf_b);
    TIME_C("float 1/b fast_recip (table+Newton)", vf_sink = fast_recip(vf_b));
    TIME_C("float 1/b recip0.s + 1 Newton", vf_sink = hw_recip1(vf_b));
    TIME_C("float 1/b recip0.s + 2 Newton", vf_sink = hw_recip2(vf_b));
    TIME_C("1/sqrt bits + 2 Newton", vf_sink = bit_rsqrt(vf_a));
    TIME_C("1/sqrt rsqrt0.s + 1 Newton", vf_sink = hw_rsqrt(vf_a));
    TIME_C("sqrtf", vf_sink = sqrtf(vf_a));
    TIME_C("1/sqrtf", vf_sink = 1.0f / sqrtf(vf_a));
    TIME_C("sinf", vf_sink = sinf(vf_a));
    TIME_C("cosf", vf_sink = cosf(vf_a));
    TIME_C("atan2f", vf_sink = atan2f(vf_a, vf_b));
    TIME_C("expf", vf_sink = expf(vf_a));
    TIME_C("logf", vf_sink = logf(vf_a));
    TIME_C("log2f", vf_sink = log2f(vf_a));
    TIME_C("powf", vf_sink = powf(vf_a, vf_b));
    TIME_C("floorf", vf_sink = floorf(vf_a));
    TIME_C("ceilf", vf_sink = ceilf(vf_a));
    TIME_C("lrintf", vi_sink = lrintf(vf_a * 1000.0f));
    TIME_C("(int)float", vi_sink = (int)(vf_a * 1000.0f));
    TIME_C("(float)int", vf_sink = (float)vi_a);
    TIME_C("int32 a/b (quos)", vi_sink = vi_a / vi_b);
    TIME_C("int32 a%b", vi_sink = vi_a % vi_b);
    TIME_C("int32 a/7 (constant)", vi_sink = vi_a / 7);
    TIME_C("int64 a*b", vl_sink = vl_a * vl_b);
    TIME_C("int64 a/b (__divdi3)", vl_sink = vl_a / vl_b);
    TIME_C("int64 (a*b)>>16 from int32", vi_sink = (int32_t)(((int64_t)vi_a * vi_b) >> 16));
    TIME_C("double a+b (soft)", vd_sink = vd_a + vd_b);
    TIME_C("double a*b (soft)", vd_sink = vd_a * vd_b);
    TIME_C("double a/b (soft)", vd_sink = vd_a / vd_b);
    TIME_C("double sqrt (soft)", vd_sink = sqrt(vd_a));
    TIME_C("float->double->float", vf_sink = (float)((double)vf_a * 1.0000001));
    TIME_C("call of an empty function", vi_sink = empty_call(vi_a));
}

/* memory: block copies and fills, and random reads that miss the cache */
static void mem_tests(void)
{
    const size_t big = 64 * 1024;
    uint8_t *i0 = heap_caps_aligned_alloc(16, big, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *i1 = heap_caps_aligned_alloc(16, big, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *p0 = heap_caps_aligned_alloc(64, big, MALLOC_CAP_SPIRAM);
    uint8_t *p1 = heap_caps_aligned_alloc(64, big, MALLOC_CAP_SPIRAM);
    const size_t chase_bytes = 2u << 20;
    uint32_t *chase = heap_caps_aligned_alloc(64, chase_bytes, MALLOC_CAP_SPIRAM);
    if (!i0 || !i1 || !p0 || !p1 || !chase) { printf("bench: no memory for the memory tests\n"); return; }
    memset(i0, 1, big); memset(i1, 2, big); memset(p0, 3, big); memset(p1, 4, big);
    struct { const char *what; uint8_t *dst, *src; } copies[] = {
        { "memcpy internal -> internal", i1, i0 }, { "memcpy PSRAM -> PSRAM", p1, p0 },
        { "memcpy internal -> PSRAM", p0, i0 }, { "memcpy PSRAM -> internal", i0, p0 },
    };
    for (unsigned k = 0; k < 4; ++k) {
        const uint32_t t0 = CYCLES();
        memcpy(copies[k].dst, copies[k].src, big);
        printf("bench mem %-36s %8.2f cycles/byte\n", copies[k].what, (double)(CYCLES() - t0) / big);
    }
    {
        uint32_t t0 = CYCLES(); memset(i0, 0, big);
        printf("bench mem %-36s %8.2f cycles/byte\n", "memset internal", (double)(CYCLES() - t0) / big);
        t0 = CYCLES(); memset(p0, 0, big);
        printf("bench mem %-36s %8.2f cycles/byte\n", "memset PSRAM", (double)(CYCLES() - t0) / big);
    }
    /* random pointer chase: one load waits for the one before; regions of 16 KB up to 2 MB */
    static const size_t regions[] = { 16u << 10, 32u << 10, 64u << 10, 256u << 10, 2u << 20 };
    for (unsigned r = 0; r < sizeof regions / sizeof regions[0]; ++r) {
        /* a random cycle through the region's cache lines (the first word of each points at the next) */
        static uint16_t perm[32768];
        const size_t n = regions[r] / 64;
        uint32_t seed = 12345;
        for (size_t i = 0; i < n; ++i) perm[i] = (uint16_t)i;
        for (size_t i = n - 1; i > 0; --i) {
            seed = seed * 1664525u + 1013904223u;
            const size_t j = (seed >> 8) % (i + 1);
            const uint16_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (size_t i = 0; i < n; ++i) chase[perm[i] * 16] = (uint32_t)(uintptr_t)&chase[perm[(i + 1) % n] * 16];
        const uint32_t *p = &chase[perm[0] * 16];
        const unsigned steps = 20000;
        for (unsigned i = 0; i < n; ++i) p = (const uint32_t *)(uintptr_t)*p;   /* warm */
        const uint32_t t0 = CYCLES();
        for (unsigned i = 0; i < steps; ++i) p = (const uint32_t *)(uintptr_t)*p;
        const uint32_t c = CYCLES() - t0;
        printf("bench mem %-28s %5u KB %8.1f cycles/load%s\n", "PSRAM random read, region", (unsigned)(regions[r] >> 10),
               (double)c / steps, p ? "" : " ");
    }
    /* the same in internal RAM (64 KB) */
    {
        const size_t lines = big / 64;
        uint32_t *w = (uint32_t *)i1;
        static uint16_t perm[1024];
        uint32_t seed = 777;
        for (size_t i = 0; i < lines; ++i) perm[i] = (uint16_t)i;
        for (size_t i = lines - 1; i > 0; --i) {
            seed = seed * 1664525u + 1013904223u;
            const size_t j = (seed >> 8) % (i + 1);
            const uint16_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (size_t i = 0; i < lines; ++i) w[perm[i] * 16] = (uint32_t)(uintptr_t)&w[perm[(i + 1) % lines] * 16];
        const uint32_t *p = &w[perm[0] * 16];
        const uint32_t t0 = CYCLES();
        for (unsigned i = 0; i < 20000; ++i) p = (const uint32_t *)(uintptr_t)*p;
        printf("bench mem %-28s %5u KB %8.1f cycles/load%s\n", "internal random read, region", 64u,
               (double)(CYCLES() - t0) / 20000, p ? "" : " ");
    }
    heap_caps_free(i0); heap_caps_free(i1); heap_caps_free(p0); heap_caps_free(p1); heap_caps_free(chase);
}

static void op_tests(void)
{
    static uint32_t words_dram[4] __attribute__((aligned(16)));
    uint32_t *const word_dram_p = words_dram;
    static float float_dram = 1.0f;
    uint32_t *word_psram = heap_caps_aligned_alloc(64, 64, MALLOC_CAP_SPIRAM);
    uint8_t *stream_int = heap_caps_aligned_alloc(16, 16 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *stream_ps = heap_caps_aligned_alloc(64, 16 * 1024, MALLOC_CAP_SPIRAM);
    words_dram[0] = (uint32_t)(uintptr_t)words_dram;
    if (word_psram) memset(word_psram, 0, 64);
#define RUN(n, s) printf("bench op  %-36s %8.2f\n", s, run(bench_##n, 1000, NULL, 1));
    OPS(RUN)
#undef RUN
    struct { const char *mem; void *word, *stream; } where[] = { { "internal", word_dram_p, stream_int },
                                                               { "PSRAM", word_psram, stream_ps } };
    for (unsigned k = 0; k < 2; ++k) {
        if (!where[k].word || !where[k].stream) continue;
        *(uint32_t *)where[k].word = (uint32_t)(uintptr_t)where[k].word;
        char label[64];
#define RUNM(n, s) \
        snprintf(label, sizeof label, "%s, %s", s, where[k].mem); \
        printf("bench op  %-36s %8.2f\n", label, \
               run(bench_##n, !strcmp(#n, "s16_stream") || !strcmp(#n, "ld_st16") ? 256 : \
                              !strcmp(#n, "s32_stream") ? 128 : 1000, \
                   !strcmp(#n, "s16_stream") || !strcmp(#n, "s32_stream") || !strcmp(#n, "ld_st16") ? where[k].stream : \
                   !strcmp(#n, "lsi_use") ? (k ? where[k].word : (void *)&float_dram) : where[k].word, 1));
        MEMOPS(RUNM)
        *(uint32_t *)where[k].word = (uint32_t)(uintptr_t)where[k].word;
#undef RUNM
    }
    heap_caps_free(word_psram); heap_caps_free(stream_int); heap_caps_free(stream_ps);
}

static void bench_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("bench: ESP32-S3 at %u MHz, core %d; numbers are cycles per body (op) or per call (c)\n",
           (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, xPortGetCoreID());
    op_tests();
    c_tests();
    accuracy();
    mem_tests();
    void sw3d_kernel_bench(void);
    sw3d_kernel_bench();
    printf("bench: done\n");
    vTaskDelete(NULL);
}

void daytona_bench_start(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, 1);
}
