/* The game loop of enDjinn without a console: the current mode's updater runs once per frame until
   something flags shutdown. In this shim a frame is exactly one simulation tick of the virtual clock
   (17,384 us), so a headless run is deterministic and as fast as the CPU allows. The interactive
   EspRacer build replaces enj_state_run with the real-time loop (main/frame_esp.c). */
#include <enDjinn/enj_enDjinn.h>
#include "frame_clock.h"
#include "practice.h"
#ifdef DAYTONA_REALTIME
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static enj_state_t state;
void (*daytona_frame_end_hook)(void);   /* after each updater call: the frame's 2D layer (shim/frame_ui.c) */
static enj_mode_t *current;
static uint64_t virtual_ns;

enj_state_t *enj_state_get(void) { return &state; }
void enj_state_flag_shutdown(void *unused) { (void)unused; state.flags.shut_down = 1; }
int enj_mode_set(enj_mode_t *mode) { current = mode; return 1; }
int enj_mode_push(enj_mode_t *mode) { current = mode; return 1; }
uint32_t daytona_frame_no, daytona_hud_frame = 0xffff0000u;   /* updater calls; the last one with a race tick */
#ifdef DAYTONA_GAME
/* The pad, made fresh before each updater call (pad_poll). The game reads it as a Dreamcast controller, so its
   modes stay unchanged:
     tilt  -> the stick (joyx): steering, and the choices of the select and name screens;
     BOOT  -> the right trigger (accelerator; the select's trigger confirm), and outside a race START and A
              (credit, confirm, enter a letter);
     PWR   -> the left trigger (brake); with BOOT too, brake only (both held 2 s is the console's exit);
     touch -> outside a race START and A on lift; in a race, top half: next view (Y), bottom left/right: gear
              down/up (X/A, manual gearbox).
   In a race START would be the recovery request and A a gear change, so BOOT gives neither there. A race is
   running while its HUD ticks (nm_hud_render_tick, shim/stubs_ui.c). */
static enj_ctrlr_state_t pad;
static enj_ctrlr_state_t *pad_list[1] = { &pad };
static unsigned pad_edge(unsigned was, unsigned now)
{
    return now ? (was ? ENJ_BUTTON_DOWN : ENJ_BUTTON_DOWN_THIS_FRAME) : (was ? ENJ_BUTTON_UP_THIS_FRAME : ENJ_BUTTON_UP);
}
static int pad_racing(void);
static void pad_set(int joyx, unsigned gas, unsigned brake, unsigned start, unsigned a, unsigned x, unsigned y)
{
    const enj_ctrlr_state_t was = pad;
    /* Every race starts on the follow camera (VR3, behind the car), as if its button were pressed: a credit
       taken during the V.R. demo would otherwise keep the demo's view of the moment (often the driver's). */
    static int was_racing, follow_frames;
    const int racing = pad_racing();
    if (racing && !was_racing) follow_frames = 30;
    was_racing = racing;
    pad.button.DOWN = pad_edge(was.button.DOWN & 1u, follow_frames > 0);
    if (follow_frames > 0) --follow_frames;
    pad.joyx = (int8_t)(joyx < -128 ? -128 : joyx > 127 ? 127 : joyx);
    pad.rtrigger = (uint8_t)gas; pad.ltrigger = (uint8_t)brake;
    pad.button.START = pad_edge(was.button.START & 1u, start);
    pad.button.A = pad_edge(was.button.A & 1u, a);
    pad.button.X = pad_edge(was.button.X & 1u, x);
    pad.button.Y = pad_edge(was.button.Y & 1u, y);
}
static int pad_racing(void) { return daytona_frame_no - daytona_hud_frame <= 2u; }   /* a race tick lately */
#ifdef ESP_PLATFORM
#include "board.h"
#include "console_esp.h"
#define TILT_FULL 0.45f    /* g of tilt for full lock (porter guide 2.5) */
#define TILT_DEAD 0.03f
static void pad_poll(void)
{
    static int primed;
    static float x0, xs;
    float g[3];
    if (board_accel(g)) {
        if (!primed) { primed = 1; x0 = g[0] > 0.2f ? 0.2f : g[0] < -0.2f ? -0.2f : g[0]; xs = 0.0f; }
        float x = g[0] - x0;
        x = x > TILT_DEAD ? x - TILT_DEAD : x < -TILT_DEAD ? x + TILT_DEAD : 0.0f;
        xs += 0.6f * (x - xs);   /* smoothing at the picture rate */
    }
    int joyx = (int)(xs * (127.0f / (TILT_FULL - TILT_DEAD)));
    if (daytona_remote.tilt) joyx = daytona_remote.tilt * 127 / 3;   /* USB console (main/console_esp.c) */
    const int boot = board_boot_down() || daytona_remote_boot(), pwr = board_pwr_down() || daytona_remote_pwr();
    int tx, ty;
    const int touch = board_touch(&tx, &ty) || daytona_remote_touch(&tx, &ty);
    static int touched, tap_x, tap_y;
    const int racing = pad_racing();
    unsigned start = 0, a = 0, x = 0, y = 0;
    if (racing) {
        if (touch) {   /* held while the finger is down */
            if (ty < LCD_H / 2) y = 1;
            else if (tx < LCD_W / 2) x = 1;
            else a = 1;
        }
    } else {
        start = a = boot;
        if (touched && !touch) start = a = 1;   /* on lift (porter guide 8) */
    }
    if (touch) { tap_x = tx; tap_y = ty; }
    (void)tap_x; (void)tap_y;
    touched = touch;
    pad_set(joyx, boot && !pwr ? 255u : 0u, pwr ? 255u : 0u, start, a, x, y);
    if (pad.button.START == ENJ_BUTTON_DOWN_THIS_FRAME || pad.button.A == ENJ_BUTTON_DOWN_THIS_FRAME ||
        pad.button.X == ENJ_BUTTON_DOWN_THIS_FRAME || pad.button.Y == ENJ_BUTTON_DOWN_THIS_FRAME)
        printf("pad: frame %u%s%s%s%s%s\n", (unsigned)daytona_frame_no, racing ? " (race)" : "",
               pad.button.START == ENJ_BUTTON_DOWN_THIS_FRAME ? " START" : "", pad.button.A == ENJ_BUTTON_DOWN_THIS_FRAME ? " A" : "",
               pad.button.X == ENJ_BUTTON_DOWN_THIS_FRAME ? " X" : "", pad.button.Y == ENJ_BUTTON_DOWN_THIS_FRAME ? " Y" : "");
}
#else
/* Host: DAYTONA_INPUT="frame:event,..." with events S, A, X, Y (pressed for that one updater call) and G=n, B=n,
   J=n (accelerator, brake, stick: held from that call on). */
#include <stdlib.h>
#include <string.h>
static void pad_poll(void)
{
    static int gas, brake, joyx;
    unsigned start = 0, a = 0, x = 0, y = 0;
    const char *spec = getenv("DAYTONA_INPUT");
    for (const char *p = spec; p && *p;) {
        char *end;
        const unsigned long f = strtoul(p, &end, 10);
        if (*end != ':') break;
        p = end + 1;
        const char ev = *p;
        long v = 0;
        if (p[1] == '=') v = strtol(p + 2, &end, 10); else end = (char *)p + 1;
        if (f == daytona_frame_no) {
            if (ev == 'S') start = 1; else if (ev == 'A') a = 1; else if (ev == 'X') x = 1; else if (ev == 'Y') y = 1;
            else if (ev == 'G') gas = (int)v; else if (ev == 'B') brake = (int)v; else if (ev == 'J') joyx = (int)v;
        }
        p = strchr(end, ',');
        if (p) ++p;
    }
    pad_set(joyx, (unsigned)gas, (unsigned)brake, start, a, x, y);
}
#endif
size_t enj_ctrl_states_length(void) { return 1; }
enj_ctrlr_state_t **enj_ctrl_get_states(void) { return pad_list; }
#else
size_t enj_ctrl_states_length(void) { return 0; }
enj_ctrlr_state_t **enj_ctrl_get_states(void) { return 0; }
#endif
#ifdef DAYTONA_REALTIME
#include "esp_timer.h"
uint64_t timer_ns_gettime64(void) { return (uint64_t)esp_timer_get_time() * 1000u; }
#else
uint64_t timer_ns_gettime64(void) { return virtual_ns; }
#endif
int pvr_get_stats(pvr_stats_t *stats) { (void)stats; return -1; }
/* "Video memory" free: the textures of the 2D screens live in PSRAM (shim/pvr_lite.c) */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
size_t pvr_mem_available(void) { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
#else
size_t pvr_mem_available(void) { return 8u << 20; }
#endif

void enj_state_run(void)
{
    while (!state.flags.shut_down && current) {
        virtual_ns += (uint64_t)NM_PRACTICE_TICK_US * 1000u;
#ifdef DAYTONA_GAME
        pad_poll();
#endif
        current->mode_updater(current);
        if (daytona_frame_end_hook) daytona_frame_end_hook();
        ++daytona_frame_no;
#ifdef DAYTONA_REALTIME
        vTaskDelay(1);   /* let the idle task and the lower-priority tasks run */
#endif
    }
}

#ifdef DAYTONA_REALTIME
/* Real time: the 17,384 us ticks that have passed since the last call (the callers cap the catch-up at 4).
   The first call after a reset primes the clock and runs none, as on the Dreamcast. */
static int64_t clock_last_us, clock_rest_us;
/* For the renderer's readout: when the last updater asked for its ticks, and how many it got. */
uint32_t daytona_updater_start_us, daytona_updater_ticks;
uint32_t nm_frame_ticks(nm_frame_clock_t *clock)
{
    const int64_t now = esp_timer_get_time();
    daytona_updater_start_us = (uint32_t)now;
    if (!clock->primed) { clock->primed = 1; clock_last_us = now; clock_rest_us = 0; return 0; }
    clock_rest_us += now - clock_last_us;
    clock_last_us = now;
#ifdef DAYTONA_FIXED_TICKS
    /* measurement: the same number of ticks every picture, so that two builds draw the same scenes */
    const uint32_t ticks = DAYTONA_FIXED_TICKS;
    clock_rest_us = 0;
#else
    const uint32_t ticks = (uint32_t)(clock_rest_us / NM_PRACTICE_TICK_US);
    clock_rest_us -= (int64_t)ticks * NM_PRACTICE_TICK_US;
#endif
    daytona_updater_ticks = ticks;
    return ticks < NM_FRAME_TICKS_LIMIT ? ticks : NM_FRAME_TICKS_LIMIT;
}
#else
/* One tick per frame; the first call after a reset only primes the clock, as on the console. */
uint32_t nm_frame_ticks(nm_frame_clock_t *clock)
{
    if (!clock->primed) { clock->primed = 1; return 0; }
    return 1;
}
#endif

/* Render-list callbacks: shim/pvr_lite.c queues them (drawn only in the game build, shim/frame_ui.c). */
void enj_render_post_callback_set(void (*callback)(void *), void *data) { (void)callback; (void)data; }
void enj_qfont_write(const char *text, int x, int y, int list) { (void)text; (void)x; (void)y; (void)list; }
void enj_qfont_color_set(int r, int g, int b) { (void)r; (void)g; (void)b; }
void enj_font_scale_set(int scale) { (void)scale; }
