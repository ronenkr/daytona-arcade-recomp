/* The 3D render calls as no-ops (headless runs); sw3d/sw3d.c implements them for real. */
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


void nm_damage_bodies_unload(nm_damage_bodies_t *bodies) { (void)bodies; }
void nm_pack_unload(nm_pack_t *pack) { (void)pack; }
int nm_pack_load(nm_pack_t *pack, const char *path) { (void)pack; (void)path; return 1; }
#ifndef DAYTONA_UI
void nm_palette_ram_defer(void) {}
int nm_palette_ram_flush(void) { return 1; }
#endif
int nm_practice_render_enqueue(nm_practice_scene_t *scene) { (void)scene; return 1; }
int nm_practice_render_load(nm_practice_scene_t *scene, const char *root, const char **stage) { (void)scene; (void)root; (void)stage; return 1; }
void nm_practice_render_release(void) {}
int nm_practice_render_tunnel(nm_practice_scene_t *scene, int tunnel, int others_only) { (void)scene; (void)tunnel; (void)others_only; return 1; }
void nm_practice_render_unload(nm_practice_scene_t *scene) { (void)scene; }
