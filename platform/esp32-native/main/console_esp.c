/* Commands on the USB serial port (porter guide 8), one letter each, for remote tests (EspRacer
   tools/racerctl.py: `shot FILE.png`, `key LETTER`):
     S        a screenshot of the last picture sent to the LCD: SHOT w h, a base64 line per row (RGB565), END
     n / p    BOOT / PWR pressed for 300 ms        g / b   BOOT / PWR held, or released again
     l / r    tilt left / right (a third of full lock), L / R full lock, c centre (the IMU again)
     t        a touch tap in the middle             v       a touch tap in the top half (next view in a race)
     D        a backtrace of every task (also printed once by itself when the game's updater stops for 3 s)
   The pad (shim/enj_shim.c) adds these to the real buttons, tilt and touch. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_debug_helpers.h"
#include <stdlib.h>
#include "board.h"
#include "console_esp.h"

daytona_remote_t daytona_remote;
const uint16_t *daytona_shown;   /* the picture last handed to the LCD (render_main.c) */
int daytona_shown_w, daytona_shown_h;

static int64_t ms(void) { return esp_timer_get_time() / 1000; }

static void out(const void *p, size_t n) { usb_serial_jtag_write_bytes(p, n, portMAX_DELAY); }

static size_t base64(const uint8_t *in, size_t n, char *o)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        o[k++] = t[v >> 18 & 63]; o[k++] = t[v >> 12 & 63];
        o[k++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    return k;
}

static void screenshot(void)
{
    const uint16_t *shown = daytona_shown;
    if (!shown) { out("ERR no picture\n", 15); return; }
    /* a copy first: sending takes about a second, while the buffer is drawn into again two pictures later */
    uint16_t *copy = heap_caps_malloc(sizeof(uint16_t) * (size_t)daytona_shown_w * (size_t)daytona_shown_h, MALLOC_CAP_SPIRAM);
    if (copy) memcpy(copy, shown, sizeof(uint16_t) * (size_t)daytona_shown_w * (size_t)daytona_shown_h);
    const uint16_t *px = copy ? copy : shown;
    char line[4 * 284 * 2 / 3 + 16];
    int n = snprintf(line, sizeof line, "\nSHOT %d %d\n", daytona_shown_w, daytona_shown_h);
    out(line, (size_t)n);
    for (int y = 0; y < daytona_shown_h; ++y) {
        size_t k = base64((const uint8_t *)(px + y * daytona_shown_w), (size_t)daytona_shown_w * 2u, line);
        line[k++] = '\n';
        out(line, k);
    }
    out("END\n", 4);
    free(copy);
}

extern uint32_t daytona_frame_no;   /* shim/enj_shim.c: updater calls */
static void console_task(void *arg)
{
    (void)arg;
    uint32_t seen = 0;
    int64_t since = ms();
    int reported = 0;
    for (;;) {
        uint8_t c;
        const int got = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) == 1;
        if (daytona_frame_no != seen) { seen = daytona_frame_no; since = ms(); reported = 0; }
        else if (seen && !reported && ms() - since > 3000) {
            reported = 1;
            printf("console: the updater has not run for 3 s (call %u); every task's backtrace:\n", (unsigned)seen);
            esp_backtrace_print_all_tasks(24);
        }
        if (!got) continue;
        daytona_remote_t *r = &daytona_remote;
        switch (c) {
        case 'S': screenshot(); break;
        case 'D': esp_backtrace_print_all_tasks(24); break;
        case 'n': r->boot_until_ms = ms() + 300; break;
        case 'p': r->pwr_until_ms = ms() + 300; break;
        case 'g': r->boot_held ^= 1; break;
        case 'b': r->pwr_held ^= 1; break;
        case 'l': r->tilt = -1; break;
        case 'r': r->tilt = 1; break;
        case 'L': r->tilt = -3; break;
        case 'R': r->tilt = 3; break;
        case 'c': r->tilt = 0; break;
        case 't': r->touch_until_ms = ms() + 150; r->touch_x = LCD_W / 2; r->touch_y = LCD_H * 3 / 4; break;
        case 'v': r->touch_until_ms = ms() + 150; r->touch_x = LCD_W / 2; r->touch_y = LCD_H / 4; break;
        default: break;
        }
    }
}

int daytona_remote_boot(void) { return daytona_remote.boot_held || ms() < daytona_remote.boot_until_ms; }
int daytona_remote_pwr(void) { return daytona_remote.pwr_held || ms() < daytona_remote.pwr_until_ms; }
int daytona_remote_touch(int *x, int *y)
{
    if (ms() >= daytona_remote.touch_until_ms) return 0;
    *x = daytona_remote.touch_x; *y = daytona_remote.touch_y;
    return 1;
}

void daytona_console_start(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t uc = { .tx_buffer_size = 4096, .rx_buffer_size = 1024 };
        if (usb_serial_jtag_driver_install(&uc) != ESP_OK) return;
    }
    /* printf through the driver too: with the driver installed beside the default console path, both drove the
       port and the game's printf could block for good (the game froze while the commands still answered). The
       driver's console drops output after 50 ms when no host reads, so playing without a PC never waits. */
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
}
