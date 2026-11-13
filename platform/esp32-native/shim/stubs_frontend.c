/* The game build (DAYTONA_GAME): the front end's render calls that have no console version yet. Each does
   nothing and reports success, so the modes run their logic and timing unchanged. */
#include <stdint.h>
#include <stddef.h>
#include "meshpack.h"
#include "render/ui/title_render.h"
#include "render/ui/select_render.h"
#include "render/ui/name_render.h"
#include "render/ui/name_labels.h"

int nm_title_render_load(void) { return 1; }
int nm_title_render_enqueue(uint64_t tick) { (void)tick; return 1; }
int nm_title_render_release(void) { return 1; }
int nm_select_render_load(const char *root) { (void)root; return 1; }
int nm_select_render_enqueue(const nm_select_t *select, uint32_t counter) { (void)select; (void)counter; return 1; }
int nm_select_render_release(void) { return 1; }
int nm_name_render_init(const nm_pack_t *pack, const nm_name_layout_t *layout, const uint16_t material_headers[2][4],
                        const uint32_t colors[2][2][64], unsigned display_mode)
{ (void)pack; (void)layout; (void)material_headers; (void)colors; (void)display_mode; return 1; }
int nm_name_render_enqueue(const nm_name_entry_t *entry, const nm_name_marker_t *marker, const nm_drive_params_t *math,
                           const int16_t center[2], uint32_t frame)
{ (void)entry; (void)marker; (void)math; (void)center; (void)frame; return 1; }
int nm_name_render_enqueue_intro(const nm_drive_params_t *math, const int16_t center[2], unsigned remaining)
{ (void)math; (void)center; (void)remaining; return 1; }
int nm_name_render_enqueue_exit(const nm_name_entry_t *entry, const nm_drive_params_t *math, const int16_t center[2],
                                unsigned phase, uint16_t tick)
{ (void)entry; (void)math; (void)center; (void)phase; (void)tick; return 1; }
int nm_name_render_release(void) { return 1; }
int nm_name_labels_load(const char *root, const nm_name_label_part_t *parts, unsigned count, unsigned static_count)
{ (void)root; (void)parts; (void)count; (void)static_count; return 1; }
int nm_name_labels_record(unsigned rank, uint32_t total, uint16_t best, int show_best)
{ (void)rank; (void)total; (void)best; (void)show_best; return 1; }
int nm_name_labels_enqueue(int seconds) { (void)seconds; return 1; }
int nm_name_labels_unload(void) { return 1; }
