/* The game's 2D layer per frame (DAYTONA_GAME with sw3d): after each updater call (shim/enj_shim.c) the
   callbacks the screens queued (enj_render_list_add) are captured (shim/pvr_lite.c) and drawn
   (shim/pvr_draw.c):
   - over the 3D picture handed to drawing in that call: with the pipeline core 1 finishes the picture, waits
     for this capture (sw3d_overlay with the picture's tag) and draws it on top; without, sw3d_finish does;
   - alone, on black, when the call drew no 3D picture (title, select; at most ~30 a second);
   - not at all when a 3D screen merely skipped a picture (its HUD would flash on black).
   pvr_wait_ready (textures about to be freed) abandons a pending wait and waits for the pipeline. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <enDjinn/enj_enDjinn.h>
#include "pvr_lite_frame.h"
#include "sw3d.h"
#include "render/materials/palette_ram.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
static int64_t now_us(void) { return esp_timer_get_time(); }
static void pause_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
#else   /* the host runs one tick per updater call: its time is the ticks' */
extern uint32_t daytona_frame_no;
static int64_t now_us(void) { return (int64_t)daytona_frame_no * 17384; }
static void pause_ms(int ms) { (void)ms; }
#endif

static pvr_lite_frame_t *volatile ready_frame[2];
static volatile uint32_t ready_tag[2];
static volatile int abandon;
static pvr_lite_frame_t *alone_frame;
static uint32_t last_posted;
static int64_t last_3d_us, last_alone_us;

static void overlay(uint16_t *view, uint16_t *depth, uint32_t tag)
{
    const pvr_lite_frame_t *f = NULL;
    if (!tag) f = alone_frame;
    else {
        const int64_t until = now_us() + 250000;
        while (ready_tag[tag & 1u] != tag && !abandon && now_us() < until) pause_ms(1);
        if (ready_tag[tag & 1u] == tag) f = ready_frame[tag & 1u];
    }
    if (f && f->count) {
        const int64_t t0 = now_us();
        pvr_lite_draw(f, view, depth, SW3D_W, SW3D_VIEW_H);
#ifdef ESP_PLATFORM
        static int64_t sum; static unsigned n;
        sum += now_us() - t0;
        if (++n == 100) {
            extern unsigned pvr_draw_stats[6];
            printf("2D layer: span loop %u cycles a pixel (%u pixels a picture)\n",
                   pvr_draw_stats[5] ? pvr_draw_stats[4] / pvr_draw_stats[5] : 0u, pvr_draw_stats[5] / n);
            printf("2D layer: %u us a picture on average (%u records); a picture: %u triangles, %u rows, %u fast pixels, %u other pixels\n",
                   (unsigned)(sum / n), f->count, pvr_draw_stats[0] / n, pvr_draw_stats[1] / n, pvr_draw_stats[2] / n, pvr_draw_stats[3] / n);
            sum = 0; n = 0; memset(pvr_draw_stats, 0, sizeof pvr_draw_stats);
        }
#else
        (void)t0;
#endif
    }
}

#ifdef ESP_PLATFORM
uint32_t sw3d_blob_hash(const uint8_t *p, size_t n);
const uint8_t *sw3d_store_blob(uint32_t hash, uint32_t bytes);
static const uint8_t *store_lookup(const uint8_t *data, uint32_t bytes) { return sw3d_store_blob(sw3d_blob_hash(data, bytes), bytes); }
#endif

static void wait_hook(void)
{
    abandon = 1;
    sw3d_wait_idle();
    abandon = 0;
}

void daytona_frame_ui_end(void);
extern void (*daytona_frame_end_hook)(void);
void daytona_frame_ui_init(void)
{
    daytona_frame_end_hook = daytona_frame_ui_end;
    nm_palette_ram_init();   /* as the Dreamcast's main.c: the palette format before any entry */
    pvr_lite_reserve();
    sw3d_overlay = overlay;
    pvr_lite_wait_hook = wait_hook;
#if defined(ESP_PLATFORM) && !defined(PVR_LITE_NO_STORE)
    pvr_lite_store_lookup = store_lookup;
#endif
#ifndef ESP_PLATFORM
    sw3d_defer_done = 1;   /* the host draws without the pipeline */
#endif
}

void daytona_frame_ui_end(void)
{
    const uint32_t posted = sw3d_posted;
    const int picture = posted != last_posted;
    last_posted = posted;
    const int64_t now = now_us();
    if (picture) {
        pvr_lite_frame_t *f = pvr_lite_capture();   /* NULL: nothing queued (drawn bare) */
        ready_frame[posted & 1u] = f;
        __atomic_store_n(&ready_tag[posted & 1u], posted, __ATOMIC_RELEASE);
        sw3d_finish();
        last_3d_us = now;
        return;
    }
    if (!pvr_lite_pending()) return;
    if (now - last_3d_us < 300000 || now - last_alone_us < 33000) { pvr_lite_discard(); return; }
    alone_frame = pvr_lite_capture();
    last_alone_us = now;
#ifdef PVR_LITE_DIAG   /* diagnostics: texture memory changed since loading */
    { static unsigned n; void pvr_lite_check(void); if (++n % 30 == 0) pvr_lite_check(); }
#endif
    sw3d_present_alone();
}
