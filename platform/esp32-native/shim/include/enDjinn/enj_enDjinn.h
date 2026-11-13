/* The part of enDjinn (Dreamcast game framework, MIT) that the native game's gameplay and mode code
   uses, for builds without KOS: the EspRacer port and host runs of the headless race. The game loop
   is in shim/enj_shim.c. The render and sound sides are no-ops here; the port's own renderer and
   mixer sit behind the nm_* functions the code calls. */
#ifndef ESP_SHIM_ENJ_H
#define ESP_SHIM_ENJ_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>   /* the real enDjinn header brings it in; mode files rely on that */
#include <stdalign.h>
#include <sh4zam/shz_sh4zam.h>   /* the real enDjinn header brings it in too */

/* The PVR list names, records and texture loader (the render side: shim/pvr_lite.c, pvr_draw.c). */
#include "pvr_lite.h"

enum { ENJ_BUTTON_UP = 0, ENJ_BUTTON_DOWN = 1, ENJ_BUTTON_DOWN_THIS_FRAME = 3, ENJ_BUTTON_UP_THIS_FRAME = 2 };

typedef struct {
    union {
        uint32_t raw;
        struct {
            uint32_t A : 2, B : 2, X : 2, Y : 2, UP : 2, DOWN : 2, LEFT : 2, RIGHT : 2, START : 2, : 14;
        };
    } button;
    int8_t joyx, joyy;
    uint8_t ltrigger, rtrigger;
} enj_ctrlr_state_t;

typedef struct {
    struct { uint32_t shut_down : 1, no_input_init : 1, no_snd_init : 1; } flags;
} enj_state_t;

typedef struct enj_mode_s {
    const char *name;
    void (*mode_updater)(void *self);
    void *data;
} enj_mode_t;

/* The render side: the PVR's TA records (pvr_lite.h, captured and drawn by shim/pvr_lite.c and pvr_draw.c). */
typedef struct { uint32_t frame_last_time, reg_last_time, rnd_last_time, vtx_buffer_used, vtx_buffer_used_max,
                 frame_count, vbl_count; float frame_rate; uint32_t enabled_list_mask, reserved[4]; } pvr_stats_t;

/* The PVR's modifier-volume and direct-render pieces the shadow code names; no-ops here (the console draws
   its shadows itself from the scene's footprints). */
#define ENJ_XSCALE 1
#define PVR_MODIFIER_OTHER_POLY 0
#define PVR_MODIFIER_INCLUDE_LAST_POLY 1
typedef struct { uint32_t cmd, mode1, mode2, mode3, pad[4]; } pvr_mod_hdr_t;
typedef struct { uint32_t flags; float ax, ay, az, bx, by, bz, cx, cy, cz; uint32_t pad[3]; } pvr_modifier_vol_t;
static inline void pvr_mod_compile(pvr_mod_hdr_t *h, int list, int mode, int cull) { (void)list; (void)mode; (void)cull; h->cmd = 0; }
static inline void pvr_set_shadow_scale(int enable, float scale) { (void)enable; (void)scale; }
static inline void enj_draw_pvr_dr64_init(void **first, void **second) { static pvr_modifier_vol_t v[2]; *first = v; *second = v + 1; }
static inline void enj_draw_pvr_dr64_commit_1st(void) {}
static inline void enj_draw_pvr_dr64_commit_2nd(void) {}

/* KOS calls the game makes outside the render side. */
int pvr_get_stats(pvr_stats_t *stats);      /* nonzero: no statistics */
size_t pvr_mem_available(void);
uint64_t timer_ns_gettime64(void);

enj_state_t *enj_state_get(void);
void enj_state_flag_shutdown(void *unused);
void enj_state_run(void);
int enj_mode_set(enj_mode_t *mode);
int enj_mode_push(enj_mode_t *mode);
size_t enj_ctrl_states_length(void);
enj_ctrlr_state_t **enj_ctrl_get_states(void);
void enj_render_list_add(int list, void (*callback)(void *), void *data);
void enj_render_post_callback_set(void (*callback)(void *), void *data);
void enj_qfont_write(const char *text, int x, int y, int list);
void enj_qfont_color_set(int r, int g, int b);
void enj_font_scale_set(int scale);
#endif
