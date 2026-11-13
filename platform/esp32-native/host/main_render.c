/* Host run of the scripted race with the software renderer: writes the frames asked for as PPM files.
     render_host ASSET_DIR OUT_PREFIX FRAME [FRAME...]   (ASSET_DIR ends in '/') */
#include <stdio.h>
#include <stdlib.h>
#include "practice.h"
#include "sw3d.h"
#include <malloc.h>

static uint16_t picture[SW3D_W * SW3D_H_PX];
static unsigned wanted[64], wanted_count, written;
static const char *prefix;

static int want(unsigned frame)
{
    for (unsigned i = 0; i < wanted_count; ++i) if (wanted[i] == frame) return 1;
    return 0;
}

static void done(const uint16_t *px, unsigned frame)
{
#ifdef DAYTONA_UI
    if (!want(frame)) return;   /* the game draws every frame (its 2D layer has its own); only these are written */
#endif
    char name[512];
    snprintf(name, sizeof name, "%s%05u.ppm", prefix, frame);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SW3D_W, SW3D_H_PX);
    for (int i = 0; i < SW3D_W * SW3D_H_PX; ++i) {
        const uint16_t c = px[i];
        const unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    const sw3d_stats_t *s = sw3d_stats();
    printf("frame %u -> %s: %u faces %u triangles %u pixels; world %u us cars %u us sky %u us\n", frame, name,
           s->faces, s->triangles, s->pixels, (unsigned)s->world_us, (unsigned)s->cars_us, (unsigned)s->sky_us);
    const struct mallinfo2 mi = mallinfo2();
    printf("heap in use %zu bytes (mmap %zu)\n", mi.uordblks, mi.hblkhd);
    if (++written == wanted_count) exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: render_host ASSET_DIR OUT_PREFIX FRAME...\n"); return 2; }
    prefix = argv[2];
    for (int i = 3; i < argc && wanted_count < 64; ++i) wanted[wanted_count++] = (unsigned)atoi(argv[i]);
    sw3d_set_target(picture);
#ifdef DAYTONA_UI
    void daytona_frame_ui_init(void);
    daytona_frame_ui_init();
#else
    sw3d_frame_wanted = want;
#endif
    sw3d_frame_done = done;
    return nm_practice_run(argv[1]);
}
