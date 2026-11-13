/* USB serial commands for remote tests (console_esp.c). */
#ifndef DAYTONA_CONSOLE_ESP_H
#define DAYTONA_CONSOLE_ESP_H
#include <stdint.h>

typedef struct {
    volatile int64_t boot_until_ms, pwr_until_ms, touch_until_ms;
    volatile int boot_held, pwr_held;
    volatile int tilt;             /* thirds of full lock, -3..3; 0: the IMU alone */
    volatile int touch_x, touch_y;
} daytona_remote_t;
extern daytona_remote_t daytona_remote;
extern const uint16_t *daytona_shown;   /* the last picture sent to the LCD, for the screenshot */
extern int daytona_shown_w, daytona_shown_h;

void daytona_console_start(void);
int daytona_remote_boot(void);
int daytona_remote_pwr(void);
int daytona_remote_touch(int *x, int *y);
#endif
