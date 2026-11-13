/* Frame-rate and memory readout (EspRacer porter guide 6.0). */
#ifndef DAYTONA_PERF_H
#define DAYTONA_PERF_H
void perf_frame(void);   /* call once per picture handed to board_lcd_draw */
void perf_memory(void);  /* one line: internal and PSRAM free / least free / largest block */
#endif
