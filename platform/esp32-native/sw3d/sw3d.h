/* The EspRacer software renderer of the native game.
 *
 * It takes the place of the Dreamcast renderer's back end: the Dreamcast scene preparation
 * (code/render/scene/practice.c: camera, poses, car parts, cell and object selection) runs unchanged and
 * fills scene->world_instances and scene->car_instances; this module loads the packs those instances
 * index and draws them into a 16-bit picture with a 1/z depth buffer. The rules come from the Dreamcast
 * path (model_submission.h, model_math.h, material_pack.h): baked world luma, lit cars, DNT6 palette
 * shading, twiddled PAL4 textures, punch-through texel alpha, the road-surface depth class and face layers.
 *
 * The picture is SW3D_W x SW3D_H_PX; the view, the logical 496x384 one scaled, is its SW3D_VIEW_H middle rows. */
#ifndef SW3D_H
#define SW3D_H
#include <stdint.h>

#ifndef SW3D_W
#define SW3D_W 284
#endif
#ifndef SW3D_H_PX
#define SW3D_H_PX 240
#endif
/* The 3D view inside the picture: 4:3 with square pixels on the LCD (as OpenMotoRacer: 284x216, 12 black rows
   above and below). The 496x384 logical view scales by 284/496 = 0.5726 across and 216/384 = 0.5625 down. */
#ifndef SW3D_VIEW_H
#define SW3D_VIEW_H 216
#endif
#define SW3D_VIEW_Y ((SW3D_H_PX - SW3D_VIEW_H) / 2)

/* The picture the next frame is drawn into (SW3D_W x SW3D_H_PX RGB565, row-major). */
void sw3d_set_target(uint16_t *pixels);
/* The console's flash texture store (sw3d.c): fill it from the card if needed, before the first load; 1 when
   in use. sw3d_store_progress, if set, is called during a fill. */
int sw3d_store_prepare(const char *root);
/* The 2D layer (sw3d.c): drawn over each picture; see there. */
extern void (*sw3d_overlay)(uint16_t *view, uint16_t *depth, uint32_t tag);
extern volatile uint32_t sw3d_posted;
extern int sw3d_defer_done;
void sw3d_wait_idle(void);
void sw3d_finish(void);
void sw3d_present_alone(void);
extern void (*sw3d_store_progress)(unsigned done, unsigned total);
/* Called after each drawn frame with the picture (host capture, console present). */
extern void (*sw3d_frame_done)(const uint16_t *pixels, unsigned frame);
/* Draw only the frames this returns nonzero for (NULL: every frame). */
extern int (*sw3d_frame_wanted)(unsigned frame);

typedef struct {
    unsigned frames, faces, triangles, pixels;   /* the last frame's faces, triangles and pixels tested */
    unsigned rows, drawn;                        /* the last frame's spans and textured pixels written */
    uint32_t raster_cycles;                      /* the last frame's cycles in the triangle filler */
    uint32_t world_us, cars_us, sky_us;
    uint32_t between_us;                         /* from the last picture's hand-over to this one's start */
    uint32_t start_us;                           /* when this frame's list build started (core 0) */
    uint32_t wait_us;                            /* PIPE: core 0 waiting for core 1's last picture */
} sw3d_stats_t;
const sw3d_stats_t *sw3d_stats(void);
#endif
