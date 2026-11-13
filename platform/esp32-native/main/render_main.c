/* DAYTONA_MODE=render: the scripted race in real time, drawn by sw3d and sent to the LCD as one rectangle per
   picture (porter guide 6.3), double buffered; perf_frame counts pictures. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board.h"
#include "racer_game.h"
#include "practice.h"
#include "sw3d.h"
#include "perf.h"
#include "esp_timer.h"
#include <string.h>
#include "console_esp.h"

static uint16_t *fb[2];
extern uint32_t daytona_updater_start_us, daytona_updater_ticks;
unsigned daytona_audio_underruns(void);

/* The Dreamcast scene set-up's entry, wrapped at link time to time it: everything from here to the renderer's
   start is the set-up (poses, parts, shadows, cell selection); before it, the game's ticks. */
struct nm_practice_scene;
int __real_nm_practice_render_enqueue(struct nm_practice_scene *scene);
static uint32_t setup_start_us;
/* Core 0's share of each picture, summed for the 50-picture averages (present() prints them, on core 1 with the
   pipeline): ticks, scene set-up, list build, waiting for core 1. */
static volatile uint32_t c0_ticks, c0_nticks, c0_setup, c0_list, c0_wait, c0_n;
int __wrap_nm_practice_render_enqueue(struct nm_practice_scene *scene)
{
    setup_start_us = (uint32_t)esp_timer_get_time();
    const uint32_t ticks_us = setup_start_us - daytona_updater_start_us, nticks = daytona_updater_ticks;
    const unsigned frames0 = sw3d_stats()->frames;
    const uint32_t start0 = sw3d_stats()->start_us;
    const int rc = __real_nm_practice_render_enqueue(scene);
    const sw3d_stats_t *s = sw3d_stats();
    if (s->start_us != start0) {   /* a picture was built */
        c0_ticks += ticks_us; c0_nticks += nticks; c0_setup += s->start_us - setup_start_us;
        c0_list += s->world_us; c0_wait += s->wait_us; ++c0_n;
    }
    (void)frames0;
    return rc;
}
static uint32_t present_end_us;
static int cur;

static void present(const uint16_t *pixels, unsigned frame)
{
    (void)frame;
    board_lcd_wait_pending(1);
    board_lcd_draw(0, 0, SW3D_W, SW3D_H_PX, pixels);
    /* The next picture is drawn into the other buffer, the one sent before this: its transfer must be over
       first (a 2D screen is drawn in a few ms, faster than the 13 ms transfer: it painted over the LCD's read) */
    board_lcd_wait_pending(1);
    daytona_shown = pixels; daytona_shown_w = SW3D_W; daytona_shown_h = SW3D_H_PX;
    cur ^= 1;
    sw3d_set_target(fb[cur]);
    perf_frame();
    present_end_us = (uint32_t)esp_timer_get_time();
    const sw3d_stats_t *s = sw3d_stats();
    /* the gap split: present..updater start, updater start..render start (ticks and the scene set-up) */
    static uint32_t gap_wait, gap_sim, gap_ticks;
    if (present_end_us) {
        const uint32_t render_start = esp_log_timestamp() ? 0 : 0;
        (void)render_start;
    }
    /* once per 3D picture count: the 2D-only screens (title, select) present without advancing it */
    static unsigned logged_frames;
    const int log_now = (s->frames % 50) == 1 && s->frames != logged_frames;
    if (log_now) logged_frames = s->frames;
    if (log_now)
        ESP_LOGI("sw3d", "frame %u: %u ticks %u us, scene set-up to here %u us; music underruns %u", s->frames,
                 (unsigned)daytona_updater_ticks, (unsigned)(setup_start_us - daytona_updater_start_us),
                 (unsigned)((uint32_t)esp_timer_get_time() - setup_start_us), daytona_audio_underruns());
    (void)gap_wait; (void)gap_sim; (void)gap_ticks;
    /* averages over the last 50 pictures (list: both cores' geometry; fill: both cores' rows and the sky) */
    /* averages over 50 pictures: core 0's parts (from the wrapper above) and the drawing and picture period here */
    static uint32_t sum_fill, sum_n, sum_frame, last_present;
    sum_fill += s->sky_us; ++sum_n;
    const uint32_t now_present = (uint32_t)esp_timer_get_time();
    if (last_present) sum_frame += now_present - last_present;
    last_present = now_present;
    if ((s->frames % 50) == 0 && c0_n) {
        const uint32_t n0 = c0_n;
        ESP_LOGI("sw3d", "frames %u-%u: list %u us, fill %u us on average; ticks %u us (%u.%02u per picture), scene set-up %u us, core 0 waits %u us, picture %u us",
                 s->frames - sum_n + 1, s->frames, (unsigned)(c0_list / n0), (unsigned)(sum_fill / sum_n),
                 (unsigned)(c0_ticks / n0), (unsigned)(c0_nticks / n0), (unsigned)(c0_nticks * 100u / n0 % 100u),
                 (unsigned)(c0_setup / n0), (unsigned)(c0_wait / n0), (unsigned)(sum_frame / sum_n));
        sum_fill = sum_n = sum_frame = 0;
        c0_ticks = c0_nticks = c0_setup = c0_list = c0_wait = c0_n = 0;
    }
    if (log_now)
        ESP_LOGI("sw3d", "frame %u: between %u us; list %u us; fill %u us (slower core %u us); %u faces %u tris %u rows %u px",
                 s->frames, (unsigned)s->between_us, (unsigned)s->world_us, (unsigned)s->sky_us,
                 (unsigned)(s->raster_cycles / 240u), s->faces, s->triangles, s->rows, s->pixels);
}

/* The first start fills the flash texture store from the card (sw3d_store_prepare): a bar meanwhile. */
static void store_progress(unsigned done, unsigned total)
{
    static int64_t last;
    const int64_t now = esp_timer_get_time();
    if (now - last < 200000 && done < total) return;
    last = now;
    uint16_t *f = fb[0];
    const int w = total ? (int)((uint64_t)done * 200u / total) : 0;
    board_lcd_wait();
    for (int y = 0; y < SW3D_H_PX; ++y)
        for (int x = 0; x < SW3D_W; ++x) {
            const int in_bar = y >= 112 && y < 128 && x >= 42 && x < 242;
            f[y * SW3D_W + x] = in_bar ? (x - 42 < w ? 0xffe0u : 0x2104u) : 0x0000u;
        }
    board_lcd_draw(0, 0, SW3D_W, SW3D_H_PX, f);
}

int daytona_render_race(void)
{
    for (int i = 0; i < 2; ++i) fb[i] = heap_caps_aligned_calloc(64, SW3D_W * SW3D_H_PX, 2, MALLOC_CAP_SPIRAM);
    if (!fb[0] || !fb[1]) return 1;
    sw3d_set_target(fb[0]);
    sw3d_frame_done = present;
#ifdef DAYTONA_UI
    void daytona_frame_ui_init(void);
    daytona_frame_ui_init();   /* the 2D screens over the pictures (shim/frame_ui.c) */
#endif
#ifdef DAYTONA_GAME
    void daytona_save_init(void);
    daytona_save_init();       /* records and settings on the card (main/save_sd.c) */
#endif
    char data[96];
    racer_game_path(data, sizeof data, "data", true);
    strlcat(data, "/", sizeof data);                 /* the game's asset root ends in '/' */
    sw3d_store_progress = store_progress;
    sw3d_store_prepare(data);
    perf_memory();
    const int rc = nm_practice_run(data);
    perf_memory();
    return rc;
}
