/* Daytona USA on EspRacer. M1: the skeleton. It follows the integration contract of Readme_porter.md
   section 4: board_init, racer_game_start right after, files only under the game's card folder.
   For now it checks the card and the data folder, prints the memory numbers and draws a test picture. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board.h"
#include "racer_game.h"
#include "perf.h"

static const char *TAG = "daytona";
int daytona_m2_race(void);

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static uint16_t *fb[2];

static void fill(uint16_t *f, uint16_t c)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) f[i] = c;
}

/* A visible failure instead of a black screen (guide section 11). */
static void fail_screen(uint16_t colour, const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    fill(fb[0], colour);
    board_lcd_draw(0, 0, LCD_W, LCD_H, fb[0]);
    board_lcd_wait();
}

#ifdef DAYTONA_AUDIO_TEST
#include "audio/sound.h"
unsigned daytona_audio_underruns(void);
/* Plays what the game plays, in order, and reports underruns: race track t04 (loops), a few effects, the
   announcer, the victory track, then a fade. Listen on the console's speaker. */
static void audio_test_task(void *arg)
{
    (void)arg;
    char data[96];
    racer_game_path(data, sizeof data, "data", true);
    nm_sound_init(data);
    static const struct { uint32_t cmd; int seconds; const char *what; } steps[] = {
        { 0xae1004, 12, "music: Let's Go Away (race, loops)" },
        { 0x9e0b7f, 2, "effect 9e0b" }, { 0x9e117f, 2, "effect 9e11 (crowd)" }, { 0xae2109, 2, "voice THREE" },
        { 0xae210a, 2, "voice TWO" }, { 0xae210b, 2, "voice ONE" }, { 0xae210c, 3, "voice GO" },
        { 0xae2101, 4, "announcer: lap call" }, { 0xb01a08, 6, "music fade" },
        { 0xae1005, 10, "music: victory lane" }, { 0xae1003, 6, "music: Bitchin'" }, { 0xff0000, 1, "stop" },
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        ESP_LOGI(TAG, "%s", steps[i].what);
        nm_sound_command(steps[i].cmd);
        for (int t = 0; t < steps[i].seconds * 20; ++t) { nm_sound_update(); vTaskDelay(pdMS_TO_TICKS(50)); }
        ESP_LOGI(TAG, "underruns so far %u", daytona_audio_underruns());
    }
    ESP_LOGI(TAG, "audio test finished");
    vTaskDelete(NULL);
}
#endif

#ifdef DAYTONA_RENDER
int daytona_render_race(void);
static void render_task(void *arg)
{
    (void)arg;
    daytona_render_race();
    vTaskDelete(NULL);
}
#endif

#ifdef DAYTONA_M2
static void m2_task(void *arg)
{
    (void)arg;
    daytona_m2_race();
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    board_init();
    racer_game_start();             /* straight after board_init: a reset must return to the menu */
    for (int i = 0; i < 2; i++)
        fb[i] = heap_caps_aligned_calloc(64, LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    if (!fb[0] || !fb[1]) { ESP_LOGE(TAG, "no picture memory"); return; }
    board_backlight(80);

    if (!board_sd_mount()) {
        fail_screen(RGB(160, 0, 0), "no SD card");
        return;
    }
    char data[96];
    racer_game_path(data, sizeof data, "data", true);
    struct stat st;
    ESP_LOGI(TAG, "data folder %s: %s", data, stat(data, &st) == 0 ? "present" : "missing");
    perf_memory();
#ifdef DAYTONA_AUDIO_TEST
    xTaskCreatePinnedToCore(audio_test_task, "audiotest", 8192, NULL, 5, NULL, 0);
    return;
#endif
#ifdef DAYTONA_BENCH
    void daytona_bench_start(void);
    daytona_bench_start();
    return;
#endif
#ifdef DAYTONA_RENDER
#ifdef DAYTONA_GAME
    /* PWR is the brake: held 10 s it must not switch the board off (porter guide 4.8; board_init of the next app,
       the launcher's too, switches it back on) */
    board_pwr_longpress_off(false);
    void daytona_console_start(void);
    daytona_console_start();   /* screenshots and remote keys over USB (main/console_esp.c) */
#endif
    xTaskCreatePinnedToCore(render_task, "race", 48 * 1024, NULL, 5, NULL, 0);
    return;
#endif
#ifdef DAYTONA_M2
    /* nm_practice_run goes deep (and its scene lives in PSRAM): a task with a big stack. */
    xTaskCreatePinnedToCore(m2_task, "m2", 48 * 1024, NULL, 5, NULL, 0);
    return;
#endif

    /* test picture: scrolling colour bars, one rectangle per picture, double buffered */
    for (int cur = 0, t = 0;; cur ^= 1, t++) {
        uint16_t *f = fb[cur];
        for (int y = 0; y < LCD_H; y++)
            for (int x = 0; x < LCD_W; x++)
                f[y * LCD_W + x] = RGB(((x + t * 3) & 63) * 4, (y & 63) * 4, 128);
        board_lcd_wait_pending(1);
        board_lcd_draw(0, 0, LCD_W, LCD_H, f);
        perf_frame();
        if ((t % 300) == 0) perf_memory();
        vTaskDelay(1);
    }
}
