/* The 2D screens' render calls, still stubbed: HUD, attract 2D layer, podium, results. */
#include <stdint.h>
#include <stddef.h>
#include "practice.h"
#include "scene_loader.h"
#include "gameplay/select_mode.h"
#include "gameplay/title_mode.h"
#include "audio/sound.h"
#include "audio/sfx.h"
#include "audio/race_sound_hooks.h"
#include "render/scene/practice.h"
#include "render/materials/palette_ram.h"
#include "render/ui/attract_2d_render.h"
#include "render/ui/podium_render.h"
#include "render/ui/result_render.h"
#include "render/ui/result_labels.h"
#include "render/ui/hud_render.h"
#include "render/ui/title_render.h"
#include "render/scene/damage_bodies.h"
#include "render/scene/slot_glass.h"
#include "meshpack.h"


#ifndef DAYTONA_UI   /* the game build compiles render/ui/ itself */
void nm_attract_2d_cinematic(unsigned phase, uint32_t counter, int last) { (void)phase; (void)counter; (void)last; }
void nm_attract_2d_demo(unsigned phase, uint32_t counter) { (void)phase; (void)counter; }
void nm_attract_2d_off(void) {}
void nm_attract_2d_phase2(unsigned course, const nm_race_record_t *records, unsigned view) { (void)course; (void)records; (void)view; }
int nm_attract_2d_render_enqueue(void) { return 1; }
int nm_attract_2d_render_load(const char *root) { (void)root; return 1; }
int nm_hud_render_load(const char *root) { (void)root; return 1; }
void nm_hud_render_reset(const nm_hud_t *hud) { (void)hud; }
/* Called once per race tick: the pad (shim/enj_shim.c) takes it as "a race is running". */
extern uint32_t daytona_frame_no, daytona_hud_frame;
void nm_hud_render_tick(const nm_hud_t *hud) { (void)hud; daytona_hud_frame = daytona_frame_no; }
#endif
#ifdef DAYTONA_UI
/* The real one (render/ui/hud_render.c) by the linker's --wrap: the pad still learns that a race is running. */
extern uint32_t daytona_frame_no, daytona_hud_frame;
void __real_nm_hud_render_tick(const nm_hud_t *hud);
void __wrap_nm_hud_render_tick(const nm_hud_t *hud) { daytona_hud_frame = daytona_frame_no; __real_nm_hud_render_tick(hud); }
#endif
#ifndef DAYTONA_UI
int nm_podium_render_enqueue(const nm_podium_t *podium) { (void)podium; return 1; }
int nm_podium_render_load(const char *root) { (void)root; return 1; }
int nm_result_labels_enqueue(unsigned rank, uint32_t total, int16_t best, int16_t scroll) { (void)rank; (void)total; (void)best; (void)scroll; return 1; }
int nm_result_labels_palettes_apply(void) { return 1; }
int nm_result_materials_palettes_apply(void) { return 1; }
int nm_result_render_enqueue(const nm_result_actor_t *actor, const nm_drive_params_t *math, const int16_t center[2]) { (void)actor; (void)math; (void)center; return 1; }
#endif
void nm_slot_glass_report(void) {}   /* the slot machine's glass (render/scene/slot_glass.c) is not drawn */
