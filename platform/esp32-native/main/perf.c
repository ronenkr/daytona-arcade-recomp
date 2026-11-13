#include "perf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

void perf_frame(void)
{
    static int64_t t0;
    static unsigned n;
    const int64_t now = esp_timer_get_time();
    if (!t0) t0 = now;
    n++;
    if (now - t0 >= 5000000) {
        ESP_LOGI("perf", "%.1f pictures/s", n * 1e6 / (double)(now - t0));
        t0 = now;
        n = 0;
    }
}

void perf_memory(void)
{
    ESP_LOGI("perf", "internal free %u min %u largest %u | psram free %u min %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
