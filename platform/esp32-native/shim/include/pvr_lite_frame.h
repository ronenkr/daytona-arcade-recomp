/* One frame's captured TA records (shim/pvr_lite.c) and their drawing (shim/pvr_draw.c). */
#ifndef ESP_SHIM_PVR_LITE_FRAME_H
#define ESP_SHIM_PVR_LITE_FRAME_H
#include <stdint.h>

struct pvr_lite_frame {
    uint32_t *records;          /* 8 words each */
    unsigned count, cap;
    uint32_t palette[1024];     /* palette RAM when the frame was captured */
};
typedef struct pvr_lite_frame pvr_lite_frame_t;

int pvr_lite_pending(void);                 /* callbacks are queued for this frame */
pvr_lite_frame_t *pvr_lite_capture(void);   /* run them into a capture buffer (two alternate) */
void pvr_lite_discard(void);                /* drop them */
void pvr_lite_reserve(void);                /* the capture buffers, at start-up */
const uint8_t *pvr_lite_texture(uint32_t mode3);   /* a header's texture address -> memory */
extern void (*pvr_lite_wait_hook)(void);    /* pvr_wait_ready: wait until no frame is being drawn */
/* the console's flash store: the same texture data where it is kept (no copy), or NULL */
extern const uint8_t *(*pvr_lite_store_lookup)(const uint8_t *data, uint32_t bytes);

/* Draws a capture over pixels/depth (w x h, the 640x480 screen scaled to them); depth is cleared first. */
void pvr_lite_draw(const pvr_lite_frame_t *frame, uint16_t *pixels, uint16_t *depth, int w, int h);
#endif
