/* The game's memory card on the console: the save file (code/save/game_save.c, include/save_store.h) in the game's
   own folder on the SD card, /sdcard/games/daytona/save/. Written safely (porter guide 4.5): the new contents
   to NAME.tmp, flushed and synced, then the old file removed and the new one renamed in its place (FATFS does
   not rename over a file); a NAME.tmp left by a reset between those two steps is taken as the save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "racer_game.h"
#include "save_store.h"
#include "game_save.h"

static char dir[96];

static void path_of(char *out, size_t n, const char *name, const char *suffix) { snprintf(out, n, "%s/%s%s", dir, name, suffix); }

/* a NAME.tmp without NAME: the reset came between remove and rename */
static void recover(const char *name)
{
    char p[160], t[168];
    struct stat st;
    path_of(p, sizeof p, name, ""); path_of(t, sizeof t, name, ".tmp");
    if (stat(p, &st) != 0 && stat(t, &st) == 0) rename(t, p);
}

static void *sd_find(void *context) { (void)context; return dir[0] ? dir : NULL; }
static int sd_free_blocks(void *context, void *card) { (void)context; (void)card; return 1000; }
static int sd_file_blocks(void *context, void *card, const char *name)
{
    (void)context; (void)card;
    recover(name);
    char p[160];
    struct stat st;
    path_of(p, sizeof p, name, "");
    if (stat(p, &st) != 0) return 0;
    return (int)((st.st_size + 511) / 512);
}
static int sd_read(void *context, void *card, const char *name, uint8_t **data, size_t *size)
{
    (void)context; (void)card;
    char p[160];
    path_of(p, sizeof p, name, "");
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    const int ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(buf); return -1; }
    *data = buf; *size = (size_t)n;
    return 0;
}
static void sd_release(void *context, uint8_t *data) { (void)context; free(data); }
static int sd_write(void *context, void *card, const char *name, const uint8_t *data, size_t size)
{
    (void)context; (void)card;
    char p[160], t[168];
    path_of(p, sizeof p, name, ""); path_of(t, sizeof t, name, ".tmp");
    FILE *f = fopen(t, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(t); return -1; }
    remove(p);
    if (rename(t, p) != 0) return -1;
    printf("save: %s written (%u bytes)\n", p, (unsigned)size);
    return 0;
}

static const nm_save_device_t sd_card = { NULL, sd_find, sd_free_blocks, sd_file_blocks, sd_read, sd_release, sd_write };

/* Before nm_practice_run (its nm_game_save_boot loads the save). */
void daytona_save_init(void)
{
    racer_game_path(dir, sizeof dir, "save", true);
    nm_game_save_device_set(&sd_card);
}
