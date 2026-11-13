/* Headless stand-ins for the render, loader, front-end and audio-engine functions the race code
   calls, so practice.c and results.c run unchanged without a renderer. Each returns "success, nothing
   drawn". The EspRacer build replaces the render and audio ones with the real implementations. */
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

unsigned long _arch_mem_top;

/* scene loader: no background loads; the callers fall back to loading in place */
int nm_loader_init(void) { return 1; }
int nm_loader_busy(void) { return 0; }
int nm_loader_finish(void) { return 1; }
void nm_loader_report(void) {}
int nm_loader_start(nm_loader_job_t job, void *arg, const char *name) { (void)job; (void)arg; (void)name; return 0; }

#ifndef DAYTONA_GAME
/* front end (the game build has the real modes, gameplay/*_mode.c) */
static uint32_t frontend_counter;
uint32_t nm_frontend_counter(void) { return frontend_counter; }
void nm_frontend_counter_set(uint32_t value) { frontend_counter = value; }
unsigned nm_select_course_raced(unsigned chosen) { return chosen; }
int nm_select_credit(uint32_t counter) { (void)counter; return 1; }
int nm_select_race_state(uint32_t *counter_base) { (void)counter_base; return 0; }
int nm_title_mode_start(void) { return 1; }
int nm_title_render_release(void) { return 1; }
#endif

#ifndef DAYTONA_AUDIO
/* audio engine (the hooks in code/audio/race_sound.c are real); the console's own is main/audio_esp.c */
void nm_sound_command(uint32_t command) { (void)command; }
int nm_sound_init(const char *asset_root) { (void)asset_root; return 1; }
void nm_sound_settle(void) {}
void nm_sound_shutdown(void) {}
void nm_sound_hold(int on) { (void)on; }
void nm_sound_update(void) {}
void nm_sfx_hint(const uint32_t *keys, unsigned count) { (void)keys; (void)count; }
#endif

