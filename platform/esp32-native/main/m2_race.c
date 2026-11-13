/* M2: the native game's gameplay on the console without graphics. Runs the scripted race of the
   recipe's script header through the unchanged practice.c and prints the CHECKPOINT rows, which
   are compared with the host's expected rows (tools/esp32/compare_checkpoints.py). */
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "practice.h"
#include "racer_game.h"

int daytona_m2_race(void)
{
    char data[96];
    racer_game_path(data, sizeof data, "data", true);
    ESP_LOGI("m2", "scripted race from %s", data);
    const int64_t t0 = esp_timer_get_time();
    const int rc = nm_practice_run(data);
    const int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI("m2", "race finished rc=%d in %.2f s; psram min free %u", rc, us / 1e6,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    return rc;
}
