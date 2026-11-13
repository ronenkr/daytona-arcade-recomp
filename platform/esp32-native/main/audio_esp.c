/* Sound for the EspRacer port: implements audio/sound.h and audio/sfx.h on board_audio.
 *
 * Music: the vgmrips logs rendered offline to 22,050 Hz mono 16-bit WAVs (tools/esp32/build_music.py),
 * streamed from the card by a reader task into a ring buffer; the mixer's fill callback (core 1, 256
 * frames at a time) takes samples from it. A loop plays from the log's loop point (music/tracks.tsv).
 * Effects: tools/esp32/build_sfx_pack.py's sfx/race.pak and sfx/voices.pak (8-bit samples): each effect or
 * phrase is read from the card when it is requested, by the reader task (a few ms). Commands are the
 * original program's packed three-byte ones; their meaning is the DC port's (code/audio/sound.c, sfx.c).
 * The engine, slip and skid layers (be16..be37) are not played yet.
 * The mix goes through a 200 Hz high-pass and x1.6 gain, as the guide says the small speaker needs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "audio/sound.h"
#include "audio/sfx.h"
#include "audio/music_map.h"

#define TAG "sound"
#define RATE 22050
#define RING_SAMPLES 32768                /* 1.49 s of music */
#define READ_SAMPLES 2048
#define MAX_VOICES 10
#define FADE_STEPS 32u
#define SEQUENCER_HZ 868u
#define WAV_HEADER 44

typedef struct { uint32_t command, first, samples; } pak_entry_t;
typedef struct {
    pak_entry_t *entries;
    unsigned count;
    int8_t *data;            /* resident sample data (race.pak), else NULL */
    char path[160];          /* voices.pak: read from the card */
    uint32_t data_offset;    /* file offset of the sample data */
} pak_t;

/* The codec's output volume, 0..100 (board_audio_volume; the board's default is 75). The codec maps 0..100 to
   -50..0 dB, so 0 is the quietest setting, not silence. */
#ifndef DAYTONA_VOLUME
#define DAYTONA_VOLUME 20
#endif

typedef struct { const int8_t *data; uint32_t pos, len; uint16_t gain; int8_t *owned; } voice_t;

static struct {
    int up;
    char root[160];
    /* music */
    struct { uint16_t track; uint32_t samples; int32_t loop; float gain; } tracks[46];
    int16_t *ring;
    volatile uint32_t ring_r, ring_w;      /* sample counters, free-running */
    volatile int want_track;               /* -1: no change, 0: stop, else track number */
    volatile int want_loops;
    volatile int playing_track;
    volatile int hold;
    volatile uint32_t fade_start_ms, fade_ms;   /* a fade in progress (0: none) */
    volatile uint16_t music_gain;          /* 0..256, from the fade */
    unsigned unmapped;
    /* effects */
    pak_t race, voices;
    uint8_t velocity_gain[128];
    voice_t voice[MAX_VOICES];
    /* start queue from the game's thread to the mixer */
    struct { const int8_t *data; uint32_t len; uint16_t gain; int8_t *owned; } start_q[16];
    volatile unsigned q_r, q_w;
    /* announcer phrase loads */
    QueueHandle_t phrase_q;
    nm_sfx_stats_t stats;
    unsigned underruns;
    int16_t hp_x, hp_y;
} S;

/* ------------------------------------------------------------------ paks and tables */

static int pak_open(pak_t *p, const char *path, int resident)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t head[12];
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "SFX2", 4)) { fclose(f); return 0; }
    p->count = *(uint32_t *)(head + 4);
    const uint32_t total = *(uint32_t *)(head + 8);
    p->entries = heap_caps_malloc(p->count * sizeof(pak_entry_t), MALLOC_CAP_SPIRAM);
    if (!p->entries || fread(p->entries, sizeof(pak_entry_t), p->count, f) != p->count) { fclose(f); return 0; }
    p->data_offset = 12 + p->count * sizeof(pak_entry_t);
    strncpy(p->path, path, sizeof p->path - 1);
    if (resident) {
        p->data = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
        if (!p->data || fread(p->data, 1, total, f) != total) { fclose(f); free(p->data); p->data = NULL; return 0; }
    }
    fclose(f);
    return 1;
}

static const pak_entry_t *pak_find(const pak_t *p, uint32_t command)
{
    unsigned lo = 0, hi = p->count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (p->entries[mid].command < command) lo = mid + 1; else hi = mid;
    }
    return lo < p->count && p->entries[lo].command == command ? p->entries + lo : NULL;
}

static void load_tracks(void)
{
    char path[200];
    snprintf(path, sizeof path, "%smusic/tracks.tsv", S.root);
    FILE *f = fopen(path, "r");
    if (!f) { ESP_LOGW(TAG, "no %s", path); return; }
    char line[200];
    while (fgets(line, sizeof line, f)) {
        unsigned n; unsigned long samples; long loop; float gain;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        n = (unsigned)atoi(line);
        char *p = strchr(tab + 1, '\t');                 /* title ends here */
        if (n < 1 || n > 45 || !p) continue;
        if (sscanf(p + 1, "%lu\t%ld\t%f", &samples, &loop, &gain) == 3) {
            S.tracks[n].track = (uint16_t)n; S.tracks[n].samples = (uint32_t)samples;
            S.tracks[n].loop = (int32_t)loop; S.tracks[n].gain = gain;
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ the music reader task */

/* An effect or a phrase to read from the card and start (the game thread queues, the reader task reads). */
typedef struct { pak_entry_t entry; uint16_t gain; uint8_t voice; } sound_request_t;
static FILE *race_file, *voices_file;
static FILE *music_file;
static int music_track, music_loops;
static uint32_t music_left, music_loop_at;

static void music_close(void)
{
    if (music_file) fclose(music_file);
    music_file = NULL; music_track = 0;
    S.playing_track = 0;
}

static void music_open(int track, int loops)
{
    music_close();
    if (track < 1 || track > 45 || !S.tracks[track].track) return;
    char path[200];
    snprintf(path, sizeof path, "%smusic/t%02d.wav", S.root, track);
    music_file = fopen(path, "rb");
    if (!music_file) { ESP_LOGW(TAG, "cannot open %s", path); return; }
    setvbuf(music_file, NULL, _IOFBF, 4096);
    fseek(music_file, WAV_HEADER, SEEK_SET);
    music_track = track; music_loops = loops;
    music_left = S.tracks[track].samples;
    music_loop_at = S.tracks[track].loop >= 0 ? (uint32_t)S.tracks[track].loop : 0;
    S.ring_r = S.ring_w = 0;
    S.music_gain = 256; S.fade_ms = 0;
    S.playing_track = track;
    ESP_LOGI(TAG, "music t%02d%s", track, loops ? " (loops)" : "");
}

static void reader_task(void *arg)
{
    (void)arg;
    static int16_t chunk[READ_SAMPLES];
    for (;;) {
        const int want = S.want_track;
        if (want >= 0 && !S.hold) { S.want_track = -1; if (want) music_open(want, S.want_loops); else music_close(); }
        /* phrases queued by the game thread */
        sound_request_t req;
        while (xQueueReceive(S.phrase_q, &req, 0) == pdTRUE) {
            pak_t *pak = req.voice ? &S.voices : &S.race;
            FILE **f = req.voice ? &voices_file : &race_file;
            if (!*f) *f = fopen(pak->path, "rb");
            int8_t *buf = heap_caps_malloc(req.entry.samples, MALLOC_CAP_SPIRAM);
            if (buf && *f && !fseek(*f, (long)(pak->data_offset + req.entry.first), SEEK_SET) &&
                fread(buf, 1, req.entry.samples, *f) == req.entry.samples) {
                const unsigned w = S.q_w;
                if (((w + 1) & 15) != (S.q_r & 15)) {
                    S.start_q[w & 15].data = buf; S.start_q[w & 15].len = req.entry.samples; S.start_q[w & 15].gain = req.gain;
                    S.start_q[w & 15].owned = buf; S.q_w = w + 1; buf = NULL;
                    if (req.voice) ++S.stats.voice_starts; else ++S.stats.started;
                } else ++S.stats.voice_drops;
            } else ++S.stats.voice_read_failures;
            free(buf);
        }
        if (music_file) {
            const uint32_t used = S.ring_w - S.ring_r;
            if (RING_SAMPLES - used >= READ_SAMPLES) {
                uint32_t n = music_left < READ_SAMPLES ? music_left : READ_SAMPLES;
                if (n) n = (uint32_t)fread(chunk, 2, n, music_file);
                for (uint32_t i = 0; i < n; ++i) S.ring[(S.ring_w + i) & (RING_SAMPLES - 1)] = chunk[i];
                S.ring_w += n; music_left -= n;
                if (!n || !music_left) {
                    if (music_loops && S.tracks[music_track].loop >= 0) {
                        fseek(music_file, WAV_HEADER + 2 * (long)music_loop_at, SEEK_SET);
                        music_left = S.tracks[music_track].samples - music_loop_at;
                    } else if (S.ring_r == S.ring_w) music_close();     /* played out */
                    else if (!n) { music_left = 0; vTaskDelay(pdMS_TO_TICKS(10)); }
                }
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------ the mixer (core 1 audio task) */

static void mixer_fill(int16_t *out, int frames)
{
    /* effects started since the last call */
    while ((S.q_r & 15) != (S.q_w & 15)) {
        const unsigned r = S.q_r & 15;
        for (int v = 0; v < MAX_VOICES; ++v)
            if (!S.voice[v].data) {
                S.voice[v] = (voice_t){ S.start_q[r].data, 0, S.start_q[r].len, S.start_q[r].gain, S.start_q[r].owned };
                goto started;
            }
        free(S.start_q[r].owned);                       /* no free voice: dropped */
        ++S.stats.dropped;
    started:
        S.q_r++;
    }
    int32_t mix[256];
    const uint32_t ring_w = S.ring_w;
    const int mg = S.music_gain;
    for (int i = 0; i < frames; ++i) {
        int32_t m = 0;
        if (S.ring_r != ring_w) { m = S.ring[S.ring_r & (RING_SAMPLES - 1)] * mg >> 8; ++S.ring_r; }
        else if (S.playing_track) ++S.underruns;
        mix[i] = m;
    }
    for (int v = 0; v < MAX_VOICES; ++v) {
        voice_t *vo = &S.voice[v];
        if (!vo->data) continue;
        int n = frames;
        if (vo->len - vo->pos < (uint32_t)n) n = (int)(vo->len - vo->pos);
        for (int i = 0; i < n; ++i) mix[i] += ((int32_t)vo->data[vo->pos + i] << 8) * vo->gain >> 8;
        vo->pos += (uint32_t)n;
        if (vo->pos >= vo->len) { free(vo->owned); *vo = (voice_t){0}; }
    }
    /* 200 Hz high-pass (one pole) and x1.6, clipped */
    int32_t hx = S.hp_x, hy = S.hp_y;
    for (int i = 0; i < frames; ++i) {
        const int32_t x = mix[i];
        hy = ((hy + x - hx) * 121) >> 7;   /* y = a*(y + x - x_prev), a = 121/128: 200 Hz at 22,050 */
        hx = x;
        int32_t o = hy * 16 / 10;
        if (o > 32767) o = 32767; else if (o < -32768) o = -32768;
        out[i] = (int16_t)o;
    }
    S.hp_x = (int16_t)hx; S.hp_y = (int16_t)hy;
}

/* ------------------------------------------------------------------ nm_sound_* */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

int nm_sound_init(const char *asset_root)
{
    if (S.up) return 1;
    snprintf(S.root, sizeof S.root, "%s", asset_root);
    size_t n = strlen(S.root);
    if (n && S.root[n - 1] != '/' && n + 1 < sizeof S.root) { S.root[n] = '/'; S.root[n + 1] = 0; }
    S.ring = heap_caps_malloc(RING_SAMPLES * 2, MALLOC_CAP_SPIRAM);   /* 64 KB: PSRAM (internal RAM is the renderer's) */
    S.phrase_q = xQueueCreate(16, sizeof(sound_request_t));
    if (!S.ring || !S.phrase_q) return 0;
    S.want_track = -1;
    load_tracks();
    nm_sfx_init(asset_root);
    /* Above the game task (priority 5, core 0), which yields only briefly per frame: the reader blocks on the
       card most of the time, so it costs little, and the music no longer starves while the race is drawn. */
    xTaskCreatePinnedToCore(reader_task, "snd_read", 4096, NULL, 8, NULL, 0);
    if (!board_audio_start(RATE, mixer_fill)) { ESP_LOGE(TAG, "audio start failed"); return 0; }
    board_audio_volume(DAYTONA_VOLUME);
    S.up = 1;
    ESP_LOGI(TAG, "up: %u effects, %u phrases", S.race.count, S.voices.count);
    return 1;
}

int nm_sfx_init(const char *asset_root)
{
    char path[200];
    snprintf(path, sizeof path, "%ssfx/race.pak", S.root);
    /* Streamed, not resident: the renderer needs the PSRAM (1.57 MB saved); a read takes a few ms. */
    if (!pak_open(&S.race, path, 0)) ESP_LOGW(TAG, "no %s", path);
    snprintf(path, sizeof path, "%ssfx/voices.pak", S.root);
    if (!pak_open(&S.voices, path, 0)) ESP_LOGW(TAG, "no %s", path);
    snprintf(path, sizeof path, "%ssfx/gains.bin", S.root);
    FILE *f = fopen(path, "rb");
    if (f) { if (fread(S.velocity_gain, 1, 128, f) != 128) memset(S.velocity_gain, 255, 128); fclose(f); }
    else memset(S.velocity_gain, 255, 128);
    (void)asset_root;
    S.stats.flows = S.race.count; S.stats.voices = S.voices.count;
    return S.race.count != 0;
}

static void want(int track, int loops)
{
    S.want_loops = loops;
    S.want_track = track;
    S.fade_ms = 0;
}

static void start_effect(const pak_entry_t *e, unsigned gain, int voice)
{
    const sound_request_t req = { *e, (uint16_t)gain, (uint8_t)voice };
    if (xQueueSend(S.phrase_q, &req, 0) != pdTRUE) ++S.stats.dropped;
}

void nm_sfx_command(uint32_t command)
{
    const uint8_t status = (uint8_t)(command >> 16), a = (uint8_t)(command >> 8), b = (uint8_t)command;
    if ((status & 0xf0) == 0xa0 && a >= 0x20 && a <= 0x22) {
        const pak_entry_t *e = S.race.count ? pak_find(&S.race, command) : NULL;
        if (e) { start_effect(e, 256, 0); return; }
        e = S.voices.count ? pak_find(&S.voices, command) : NULL;
        if (e) start_effect(e, 256, 1); else ++S.stats.missing;
    } else if ((status & 0xf0) == 0x90 && b) {
        const pak_entry_t *e = S.race.count ? pak_find(&S.race, command & 0xffff00u) : NULL;
        if (e) start_effect(e, 1u + (unsigned)S.velocity_gain[b & 127], 0); else ++S.stats.missing;
    }
    /* be50: stop the effects (and the engine layers); be16..be37: engine layers, not played yet */
}

void nm_sound_command(uint32_t command)
{
    if (!S.up || !command) return;
    nm_sfx_command(command);
    const uint8_t status = (uint8_t)(command >> 16), a = (uint8_t)(command >> 8), b = (uint8_t)command;
    const nm_music_entry_t *music = (status & 0xf0) == 0xa0 ? nm_music_find(command) : NULL;
    if (music) want(music->track, music->loops);
    else if ((status & 0xf0) == 0xa0 && a == 0x10) { if (++S.unmapped <= 8) ESP_LOGW(TAG, "music start %06lx has no recording", (unsigned long)command); }
    else if (status == 0xff || ((status & 0xf0) == 0xb0 && a == 0x50 && b != 0x7d)) want(0, 0);
    else if (status == 0xb0 && a == 0x07 && !b) want(0, 0);
    else if ((status & 0xf0) == 0xb0 && a == 0x1a && b && S.playing_track && !S.fade_ms) {
        S.fade_start_ms = now_ms();
        S.fade_ms = FADE_STEPS * b * 1000u / SEQUENCER_HZ;
    }
}

void nm_sound_update(void)
{
    if (!S.up) return;
    if (S.fade_ms) {
        const uint32_t elapsed = now_ms() - S.fade_start_ms;
        if (elapsed >= S.fade_ms) { S.fade_ms = 0; want(0, 0); }
        else S.music_gain = (uint16_t)(256u - elapsed * 256u / S.fade_ms);
    }
}

void nm_sound_hold(int on) { S.hold = on; }
uint32_t nm_sound_idle(uint32_t budget_us) { (void)budget_us; return 0; }
int nm_sound_loading(void) { return 0; }
void nm_sound_settle(void) {}
void nm_sound_shutdown(void) { S.up = 0; }

void nm_sfx_update(void) {}
void nm_sfx_shutdown(void (*wait_ms)(unsigned)) { (void)wait_ms; }
const nm_sfx_stats_t *nm_sfx_stats(void) { return &S.stats; }
void nm_sfx_quiesce(void) {}
void nm_sfx_hint(const uint32_t *keys, unsigned n) { (void)keys; (void)n; }
uint32_t nm_sfx_idle(uint32_t budget_us) { (void)budget_us; return 0; }

/* A readout for the test build and the perf line. */
unsigned daytona_audio_underruns(void) { return S.underruns; }

/* Big reads in pieces (linked as fread, -Wl,--wrap=fread): the course loads read whole packs in one call (a world
   mesh is 1.4 to 2.7 MB), which held the card's file-system lock for seconds while the music ring (1.5 s) ran
   dry. Between 32 KB pieces the music reader (a higher priority) gets the card. */
size_t __real_fread(void *ptr, size_t size, size_t count, FILE *file);
size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *file)
{
    const size_t bytes = size * count;
    if (bytes <= 32768u || size == 0) return __real_fread(ptr, size, count, file);
    size_t done = 0;
    while (done < bytes) {
        const size_t want = bytes - done < 32768u ? bytes - done : 32768u;
        const size_t got = __real_fread((uint8_t *)ptr + done, 1, want, file);
        done += got;
        if (got < want) break;
        taskYIELD();
    }
    return done / size;
}
