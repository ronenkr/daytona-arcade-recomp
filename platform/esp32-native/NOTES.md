# Daytona USA on EspRacer: notes

Port of `platform/dreamcast-native` to the EspRacer console (ESP32-S3-Touch-LCD-1.83). The rules are
in EspRacer's `Readme_porter.md`; the plan is milestones M0 to M7.

## Status

| Milestone | State |
| --- | --- |
| M0 toolchain and data | ROM set merged into `roms/daytona93.zip` (ignored by git) and imported; `extern/` fetched; host tools built with clang (SDL3 configured with `-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_UNIX_CONSOLE_BUILD=ON`, since only the command-line tools are needed); host `pvrtex` built from KOS 368a2eb; the asset recipe finished in 3,395 s (`build/esp-assets/recipe2.log`, directory-mode ROM, `--no-music`): 2,078 outputs byte-identical to `recipes/expected.sha256`, 1 different (`headers/hud/hud_data.h`, a HUD header, probably the newer Pillow's text rendering; not used by M2, check before the HUD is drawn), 302 not built (music, sfx and ELF rows that need AICAforge or KOS). The recommended set is `build/esp-assets/out/assets-ssfar4f` (20 MB) |
| M1 skeleton on the console | done: `project(daytona)` builds (image 0x4fc70 bytes of the 2 MB slot), launches from slot `game3`, menu card shows name, description and icon |
| M6 audio, host side | done: `tools/esp32/vgm_render.cpp` and `build_music.py` render all 45 logs to 22,050 Hz mono WAV (65.1 MB, 1,476 s, ~2.5 min) with loop points in `tracks.tsv`; the console mixer is not written |
| M2 gameplay on target, no graphics | **done**: 259 / 320 / 345 rows identical to the recipe's expected rows on the console for Beginner, Advanced and Expert (table below); earlier detail: the unchanged `practice.c`, `results.c`, sim and save code build against `shim/` (enDjinn, KOS and SH4ZAM stand-ins) both on the host (`host/Makefile`) and in ESP-IDF; a 3,764-tick coasting race to the time-out and results prints 94 CHECKPOINT rows on the console that are **identical** to the host's; the real recorded-race rows (259/320/345) wait for the recipe's script headers |
| M6 audio, console side | music confirmed audible on the console by the user (effects, announcer and underrun counts not yet reviewed): `main/audio_esp.c` implements `audio/sound.h` and `sfx.h` (streamed music with loop and fade, resident effects, announcer phrases from the card, 200 Hz high-pass and x1.6); `DAYTONA_MODE=audio` builds a test that plays what the game plays and logs underruns; engine, slip and skid layers (be16..be37) still to do. The card was filled with the staged folder from a PC (see transfer speed below) |
| M3 renderer v1 | runs on the console at 3.2 to 5.2 pictures/s with music (race effects now read from the card per play, to leave PSRAM for the renderer); on the host: correct pictures of the scripted race (world, cars, sky panorama) from `sw3d/sw3d.c`; built for the console (`DAYTONA_MODE=render`, image 0xdb730 bytes), first run waits for the render assets on the card |
| M4 speed | **closed by the owner** (2026-11-12) at about 21 pictures/s on average and 15.9 to 17.6 in the heaviest windows, full picture quality, music without underruns; the 30 target was waived (tuning log below) |
| M5 front end and input | in progress (2026-11-12). `DAYTONA_MODE=game`: the unchanged mode code (attract, V.R. demo, title, select, race, results, name entry) with the pad made from the console (shim/enj_shim.c: tilt the stick, BOOT accelerator and START/A outside a race, PWR brake, touch START/A in menus, view and gears in a race; every race starts on the follow camera). The 2D screens are the Dreamcast's own render/ui/ code: its PVR records are captured (shim/pvr_lite.c, KOS's header layout) and drawn in software over the picture or alone (shim/pvr_draw.c: 4/8-bit palette, ARGB1555/565/4444, VQ, punch-through, translucency, depth compare; shim/frame_ui.c ties them to sw3d's pipeline). Records and settings saved on the card (main/save_sd.c). Checked on the console: attract over all three courses, title, select, race with HUD and cars, results, save. The 2D layer costs about 7 ms a race picture (the HUD) and 40 ms a select picture. Not yet checked: name entry with a record, podium, settings screen (its text is enDjinn's debug font, not drawn). Fixed 2026-11-13: an olive line across the sky (sw3d sky_draw: the first screen row of the panorama's rect could sample just above it and wrap to texture row 511; rows and columns now clamped to the rect). USB console: `D` prints every task's backtrace, and it is printed by itself when the updater stops for 3 s (one freeze on the title, not reproduced). The text is the Japanese setting (COUNTRY JPN, the factory value the native art covers) |
| Memory for Advanced and Expert | the flash data partition `daytona` (EspRacer's table: game4-game6 and the tail, 6.9 MB) holds, filled from the card at the first start: the car pack's textures and material tables, Expert's, the 2D screens' textures, then Advanced's textures as far as they fit (sw3d_store_prepare). Packs with their tables there are opened without a PSRAM copy. The world and car meshes are read table by table (no 2.7 MB block; the strips and position references are skipped). PSRAM free after loading: Beginner 1.2 MB, Advanced 1.7 MB, Expert 0.3 MB. Large reads go in 32 KB pieces (`__wrap_fread`): the music streams through course loads |
| M7 | not started |

## ROM set

`daytona93.zip` from MAME is a split set: its 10 files (CRCs match `src/runtime/rom_import.cpp`) are the
ones that differ from `daytona` (Revision A). The 26 shared files come from the parent set. The merged
zip holds the 10, the shared ROMs, the MultiPCM sample ROMs and Revision A's sound program
(epr-16720/16721), the same one `tools/extract_sound_roms.py` reads. It stays in `roms/`.

## Measurements on the console

| What | Value | Run |
| --- | --- | --- |
| PSRAM free at start (skeleton, nothing allocated) | 8,107,132 bytes | M1 |
| Internal RAM free at start | 308,999 bytes (min 272,868) | M1 |
| Test picture, one full-screen rectangle, double buffered | 71.6 pictures/s | M1 |

The PSRAM figure is above the 5.2 MB the guide quotes for OMR (which counts OMR's own arrays), so the memory
plan has more room than assumed. Re-measure once the sim and the pack loaders are in.

## Tuning log (porter guide 6.0)

Heaviest scene for the target: the Beginner start grid and the first laps of the scripted race (40 cars,
grandstand). Pictures/s are 5 s windows on the console, final build, sim at real time.

| Step | Change | Pictures/s | World / cars / sky per picture |
| --- | --- | --- | --- |
| 0 | sw3d v1: one core, float bounding-box rasteriser, per-pixel divide and floor, float depth in PSRAM, per-face transforms | 3.2 to 5.2 | 120-230 / 20-40 / 12-20 ms |
| 1 | scanline spans, perspective divide every 8 pixels, masks instead of division for wrapping, 16-bit depth in internal RAM | 5.3 to 9.0 | raster 52-114 ms of it |
| - | rejected: renderer statics kept out of PSRAM and the filler in IRAM (linker.lf): no measurable change | 5.2 to 9.0 | |
| 2 | both cores fill: core 0 builds a triangle list, then core 0 fills the even rows and core 1 the odd ones (sky too) | 7.5 to 11.6 | list 12-40 ms, fill 38-72 ms |
| - | measurement only: the same without texture fetch (flat colour) | 10.6 to 15.7 | fill 23-46 ms: texturing is about a third of the fill |
| 3 | edge slopes once per triangle instead of a divide per row; ceil without libm | 9.2 to 13.0 | fill 34-58 ms |
| 4 | the 3D view at 284x216 (4:3, square pixels, 12 black rows above and below, as OpenMotoRacer) instead of the 496x384 view stretched to 284x240 | - | 10% fewer pixels; picture fix |
| 5 | textures converted at load from twiddled PAL4 to plain rows; mip level per face from the texel/pixel area ratio; triangles drawn nearest first; perspective divide every 16 pixels | 8.3 to 13.5 | fill 28-56 ms |
| 6 | triangle set-up once, when the list is built, instead of on each core; triangles with no pixel centre dropped there (40% of them) | 8.1 to 13.8 | fill 24-50 ms, list longer |
| - | measurement only: pixel writes to an internal-RAM row instead of the PSRAM picture | 9.6 to 14.1 | no real change: PSRAM writes are not the limit |
| - | finding: the pixel loop spilled about 15 values to the stack per pixel (register pressure), and 1/z called the ROM's software __divsf3 | | about 200 cycles a pixel |
| 7 | span kernels specialised by mirror and punch-through (8 compile-time variants), depth stepped as a 16.16 integer across each 16-pixel span, `-ffast-math` for sw3d.c only | 9.7 to 18.2 | fill 12-44 ms, list 4-42 ms |
| 8 | meshes and groups of 8 world faces culled by their bounding boxes before any vertex is transformed; each car face keeps its last binding lookup (no binary search over 60,616 bindings per face per frame) | 10.0 to 23.5 | list 10-30 ms |
| 9 | the geometry on both cores too: each takes alternate groups of 8 faces into its own list; both lists filled by both cores | 11.3 to 26.4 | list 7-21 ms, fill 20-47 ms |
| 10 | affine texturing (no divide per row or span) for triangles whose 1/z varies by under 12% | 11.7 to 25.9 | fill about 10% less; small texture swim on near car sides |
| - | finding: `-ffast-math` did not stop the toolchain calling the ROM's software `__divsf3` for float divides (3 call sites left in the fill) | | |
| 11 | every divide in the renderer through a 256-entry table of 1/mantissa, an exponent flip and one Newton step (relative error about 2^-17) | 13.5 to 26.4 | list 9-20 ms, fill 18-38 ms; no `__divsf3` left |
| 12 | level-of-detail groups picked per group from their switch depths (meshpack.h), instead of always level 0; the stipple fence's sweep groups left out | | 14% fewer world faces, same picture |
| - | bug found and fixed: the decals on the cars (the "10") lost their depth test. Interpolating the 16-bit depth code (logarithmic in 1/z) across a 16-pixel span bends by up to about 12 units on a near, slanted face; a decal leads its base by about 2. 1/z is linear on the screen: it is stepped per pixel as a float and coded per pixel (one add, one subtract, one shift) | | |
| 13 | the affine shortcut limited by size: only when the 1/z spread times the triangle's screen size stays under 1.5 pixels; affine planes stored in the perspective fields (PSRAM ran out with separate ones) | 13.2 to 24.9 | list 7-20 ms, fill 21-41 ms |
| 14 | span kernel, face set-up, culling and the sky in IRAM (they had been linked into flash) | 13.6 to 25.2 | a few per cent |
| - | rejected: the sky texel position stepped in 16.16 instead of a float divide per pixel: no measurable change (kept, it is simpler) | | |
| - | measurement: pixel work skipped (row and triangle overhead only): fill 9.5 ms of 30 in the heavy frame; per pixel about 84 cycles, mostly waiting on PSRAM | | |
| 15 | sort keys and the sorted order in internal RAM (the sort had read each triangle's key from its 76-byte record in PSRAM: a cache miss per triangle, twice) | 14.5 to 25.8 | 4 ms less per frame |
| - | rejected: a pipeline (core 1 draws picture N from a snapshot of its instances while core 0 runs the ticks of N+1; both take geometry chunks and 16-row bands from shared counters). The ticks slowed from about 20 to 30 ms a frame and the drawing too: the two cores share one data cache and one PSRAM bus. Memory-bound, not short of cores | 13.1 to 25.5 | |
| - | measurement: where a frame goes (heavy scene, about 14.5 pictures/s): the game's ticks 3-4 x 5-5.6 ms (4.3 ms each without rendering: the renderer evicts their data), the Dreamcast scene set-up 2-3 ms, the triangle list 15 ms, the fill 30 ms | | |
| - | measurement: a draw distance of 150 units (meshes and face groups beyond it dropped): 30% fewer faces, only the far tree line goes; 14.6 to 26.9. Too little for what it removes; the default stays the arcade's (to the sky) | | |

| 16 | depth stepped per 16-pixel span as an exact-at-three-points quadratic (two integer adds per pixel instead of a float add and bit work) | 14.5 to 27.0 | kernel benchmark 58 -> 39 cycles a drawn pixel, 41 -> 30 a hidden one |
| 17 | one row loop per texture variant with the span kernel inlined; each span's end depth reused as the next one's start; a reciprocal table for short spans | 14.5 to 27.0 | small |
| - | measurement: one 128x128 texture in internal RAM for every face: the fill barely changed. Texture fetches from PSRAM are not the limit | | |
| - | profile (cycle counter, heaviest frame, per core): fill 34.7 ms = pixel kernel 13.3 + sky 2.4 + triangle, row and span set-up about 18 (about 400 cycles a row over 8-10K rows of about 7 pixels) | | |
| - | rejected: 16-row bands from a shared counter instead of even and odd rows (each small triangle set up once): no gain, plus 2 ms to sort the row ranges | 13.5 to 26.3 | |
| 18 | incremental rows: edges and each attribute's value at x = 0 step by a constant per row (one multiply-add per attribute at x0 instead of a plane evaluation); every span's depth from the same integer quadratic (no float path for short spans); depth code without range checks | 13.9 to 26.7 | no measurable change |
| - | rejected: triangles filled in list order instead of nearest first (memory locality of their records): no change | | |
| - | profile (core 0, heaviest frame): 13,600 row iterations and 10,000 spans cost about 150 cycles a row plus 170 a span outside the pixel kernel: a float scanline on this FPU, for thousands of triangles about 7 rows tall. Removing operations from it does not move it | | |
| - | measurement: opponents drawn with the 15-face level-1 body below 24 Dreamcast rows instead of 12 (`NM_SMALL_CAR_PX`, an existing switch of render/scene/practice.c; about 11 rows of this screen): 9% fewer faces in a pack of cars, mid-distance cars lose their separate wheels and windows; 14.3 to 27.2 (+2-3%). Not taken: the default stays 12 (full quality) | | |
| - | not possible: the game's 106 KB state in internal RAM (to shield the ticks from the renderer's cache traffic): the internal data segment overflows at link time | | |
| - | measurement: what operations cost on this chip (`DAYTONA_MODE=bench`, `host/console_bench.sh`; table below). Float add, multiply and multiply-add: 1 a cycle but 4 cycles until the result can be used; a float compare and branch 4; integer add, shift, extract 1, multiply 2; a load 2 (internal RAM or a PSRAM cache hit); a PSRAM cache miss 172 | | |
| 19 | fixed-point rows: the depth code (float bits of 1/z) is exactly linear in 1/z within one binade, and 1/z is linear on the screen, so a triangle whose corners share the exponent has a depth plane; such triangles (89% of them) walk their edges in 16.16 and keep depth, u and v at each row's left pixel with a DDA (row step plus whole-pixel move times x step), all integer; perspective ones keep only the divide every 16 pixels in float. Same picture (0-41 pixels of 68,160 differ, at edges); host check: depth within 0.001 units, u/v within 0.007 texels | 14.7 to 27.0 | |
| 20 | car lighting: 1/sqrt from the float bits and two Newton steps instead of a software double `sqrt` (808 cycles) and a divide per lit face (lighting.h, `__XTENSA__` only, like the Dreamcast's fsrra path); the mip level from the exponents of the two areas instead of a 53-cycle divide | | |
| 21 | reciprocal from the FPU's own seed (`recip0.s`) and two Newton steps instead of the 256-entry table: 22 instead of 26 cycles, relative error 1.2e-7 instead of 3.8e-6 | 15.3 to 27.4 | heavy windows +5-8% over step 18 |
| - | rejected: the fixed-point row loop as two runs (above and below the middle corner) with plain left and right edges, one offset for picture and depth: same time. Per-row instruction count is not the limit | | |
| - | measurement (fixed-point path, heavy frame, core 0): an internal-RAM texture for every face saves 3 of 32 ms of fill; pixel rows written to internal RAM instead of the PSRAM picture save 1.7 ms. Cache misses are a share, not the bulk | | |
| - | rejected: 32-byte data cache lines instead of 64 (2,048 lines instead of 1,024): 14.0 to 26.0, about 5% slower. The data cache is already at the chip's largest (64 KB, 8 ways); octal PSRAM at 120 MHz is an experimental IDF option that fails with temperature drift, not tried | | |
| - | finding: no powf anywhere; sinf/cosf only in three places of the scene set-up (object, clock and road-depth placement); the game ticks call sqrtf and a float divide a few times each. The double arithmetic in geometry.c (bounds_test, project_classified_triangle) is the host reference path and has no caller on the console | | |
| - | profile (heavy frame, core 0): fill 32 ms = sky 2.7, fixed-point triangles 16.2 (2,108 triangles, 33,151 pixels: per-triangle prologue 2.8, kernel 6.1, rows and spans 7; rows average 3 pixels), float-path triangles 11.8 (265 triangles that cross a binade or are too large for 16.16); list build 13-20 ms: tri_setup about 850 cycles a triangle, the rest of a face about 860, culling, binding and paint work about 1,300 per face visited (about 300 paints set up per core per frame, about 1 ms) | | |
| - | measurement tools: `DAYTONA_DEFS` (extra definitions for a build, e.g. `"DAYTONA_FIXED_TICKS=3;SW3D_PROFILE"`); `DAYTONA_FIXED_TICKS=n` gives every picture n ticks, so two builds draw the same scenes and their list and fill averages (printed per 50 pictures) compare directly. The real-time windows vary by a few per cent from run to run and hid or invented changes of that size | | |
| - | profile of the list build (heavy frame, core 0, 13.8 ms): tri_setup 4.2, vertex transform and projection 2.0, paints 0.9 (337 set up), rear test 0.7, group boxes 0.4, car lighting about 1.5, the rest the face loop and its PSRAM reads | | |
| 22 | perspective rows on the fixed-point path whatever their binades: the depth of each 16-pixel span from its 1/z, a run cut where 1/z crosses a power of two (the depth code's slope changes there); affine triangles that cross a binade are drawn as perspective ones. 2,370 of 2,373 triangles now take the fixed-point path (before: 2,108; the rest crossed a binade). Fixes the "10" decal on the green car, which lost pixels where the old quadratic depth spanned a binade | 15.3 to 15.7 | fill 0-3% less, same scenes |
| 23 | the mirrored texture variants (none in these scenes) in flash instead of IRAM: 37.7 KB more internal RAM (IRAM comes out of the same SRAM as the heap); the first 512 paints of one list in internal RAM (the other list's do not fit) | 21.5 / 27.9 / 19.2 / 15.4 in the first windows (step 21: 21.2 / 27.4 / 18.8 / 15.3) | list 2-4% less |
| 24 | one paint buffer for both cores per frame instead of one per list: a binding gets one paint a frame, whichever core meets it first (a hash table of 2,048 slots in internal RAM, claimed by compare-and-swap; the other core waits for a paint being set up). Host count over a race: 690 to 1,420 paints were made a frame, of 330 to 750 distinct bindings; about 5,000 distinct over a whole race, too many to keep across frames. Paints 48 bytes instead of 64; as many as the largest internal block allows in internal RAM (597), the rest in PSRAM; x*m/255 by an exact shift identity instead of 48 divides a paint. PSRAM free after loading 87 -> 175 KB | 21.8 / 28.2 / 19.4 / 15.6 in the first windows; 15.6 to 16.1 in the heaviest | same scenes: list 2-4% less, fill about 1% |
| - | rejected: bands instead of even and odd rows, so that a small triangle is set up by one core only (the two cores' summed work fell about 8%). One split row per frame, moved by the cores' measured times or placed by an estimate of each row's cost: out of balance up to 2:1 in the heaviest frames (the sky and the grandstand's many small triangles sit at the top), fill 31 -> 34-36 ms. Bands of 16 rows alternating between the cores: 35 ms (big near triangles set up once per band, uneven bands). Even and odd rows stay | | |
| - | measurement (same scenes, heaviest 50 pictures, 66.4 ms a picture): game ticks 17.7 ms (3 a picture, 5.9 ms each), scene set-up 2.4, triangle list 14.8, fill 30.9. Of a tick, the race step (the 40 cars' simulation) is 4.6 ms, the camera pass 0.55, events, rows after them, smoke and HUD 0.6 together. The race step's TGP table reads (sine, arctangent, reciprocal, inverse square root: 64 KB each, random indices) wait about 90,000 cycles a tick (0.37 ms; about half the reads miss); the rest is arithmetic: the same race on the PC takes 0.4 to 0.8 million cycles a tick against 1.1 million here | | |
| - | rejected: the gameplay files at -O3 (results stay IEEE-exact without fast math or contraction): race step unchanged within 1%. The instruction cache line cannot go above 32 bytes in this IDF; the caches are at their largest sizes | | |
| - | rejected: each affine triangle's fixed-point start values (edges, depth, u, v) worked out once in tri_setup instead of on each core (bit-identical pictures, 64% of the triangles): fill 1.3 ms less, list 1.0 ms more, same picture time | | |
| - | volume: board_audio_volume(20) at audio start (`DAYTONA_VOLUME`, the owner's request; was 0): the codec maps 0..100 to -50..0 dB, so 20 is about -40 dB | | |
| - | rejected: the textured pixel loop and the whole row walk of affine triangles in assembly (every value in a register; u scaled so the texture width is 2^32, its wrap free). Instruction count per pixel about 22 instead of 30, per row about 50 instead of 85, same pictures; same fill time to 1% on the same scenes. Removed | | |
| - | measurement (same scenes): one core filling both parities takes 1.9 times as long as two: the cores do not compete for memory | | |
| - | measurement (same scenes, heaviest 50 pictures, fill 31.3 ms): triangle set-up only, no rows, 12.0 (sky 2.7 of it); with all rows but no perspective pixels 23.6. Perspective spans hold over 80% of the pixels (host count); their pixels cost about 53 cycles each against about 22 instructions: the rest is waiting on PSRAM (textures, picture lines). Triangle records and paints are read from PSRAM by both cores | | |
| - | finding: the compiler cloned fill_band (`fill_band.constprop.0`); the clone missed linker.lf's IRAM placement and ran from flash, 3 ms more fill a picture. `noinline, noclone` keeps it in IRAM | | |
| 25 | the pipeline, now the default on the console (`SW3D_NO_PIPELINE` for the old order): core 1 draws picture N, all rows, while core 0 runs the ticks, the scene set-up and the whole list of N+1. Lists, sort order and paints double-buffered (one triangle array a set that both lists share, 3,400 triangles; 896 paints a set); one picture more latency. PSRAM free after loading 65 KB | 23.0 / 30.2 / 20.7 / 17.4 / 19.7 in the first windows (step 24: 21.8 / 28.2 / 19.4 / 15.6 / 16.9); whole race 15.9 to 30.2, about 21 on average; music underruns 0 | same scenes: heaviest 50 pictures 62.8 ms (step 24: 66.3), 5-9% less. A tick slows from 5.9 to about 7.3 ms (both cores share the data cache); core 0 and core 1 come out about even in real time (core 0 waits 0-3.5 ms) |
| - | measurement (host, `SW3D_OVERDRAW`): pixels visited per picture pixel 1.0 to 2.15; in the heaviest pictures 46-48% of the visited pixels, 58-59% of the rows and 34-41% of the triangles change nothing (hidden behind nearer ones, drawn nearest first) | | |
| - | rejected: depth culling per row (the farthest depth of each 16-pixel piece of each row, a row skipped when its triangle's nearest depth is behind every piece; identical pictures). Exact pieces (worked out from the depth buffer when drawn, or when next tested): 6,000-8,000 rows skipped in heavy pictures, fill 31 -> 33 ms. Pieces raised only by opaque rows that cover them whole (no reading back): 1,000-2,100 rows skipped, fill +0.6 ms. A hidden pixel costs a depth read in internal RAM and no texture or picture access, so skipping it saves little; whole hidden triangles would need a per-triangle summary in internal RAM, which has no room | | |
| - | rejected: triangle records of one 64-byte cache line (third corner instead of the three edge slopes, row range and depth scale worked out again when drawn, identical pictures): list 4% less, fill 2% more, picture the same; pipeline: picture +1%. Consecutive records in the drawing order mostly share lines already, so the second line was rarely a miss. Keeps 88 KB more PSRAM free: an option if Expert's world needs the memory | | |

Where it stands (after step 25): about 21 pictures/s on average over the whole race, 15.9 to 17.6 in the
heaviest windows (40 cars and the grandstand). The owner accepted about 20 in the heaviest scenes (2026-11-12),
not 30, to keep the full picture, and then closed M4 at these numbers (15.9 to 17.6 in the heaviest windows,
about 21 on average): no more frame-rate work; on to the front end (M5). With the pipeline a heavy picture is about 60 ms on each core: core 0 runs
3-4 ticks (about 7.3 ms each, slowed by core 1's cache traffic), the scene set-up (2 ms) and the triangle list
(about 30 ms on one core); core 1 fills (about 60 ms on one core). Both memories are nearly full after loading
(internal RAM about 52 KB free, largest block 31 KB; PSRAM 65 KB). What is left: the list build (about 3,000
cycles a visible face) and the fill's PSRAM waits; overdraw and record size are ruled out above.

### Operation costs on the console (cycles at 240 MHz, `host/console_bench.sh`)

| Operation | Cycles | Operation | Cycles |
| --- | --- | --- | --- |
| integer add, shift, extui, nsau, clamps, min/max | 1 | float add.s / mul.s / madd.s, until the result is usable | 4 (1 a cycle when independent) |
| mull (32x32), mulsh, mul16 | 2 | recip0.s, rsqrt0.s (seed, about 7 bits) | 2 |
| quos / quou (integer divide) | 12 (4 for small operands) | float to int (trunc.s), int to float (float.s) | 1 each, plus the 4-cycle wait |
| variable shift (ssl + sll) | 2 | float compare and branch (olt.s + bt) | 4 |
| branch taken / not taken | 3 / 1 | wfr / rfr (int to float register and back) | 3 round trip |
| load, internal RAM or PSRAM cache hit, until usable | 2 | PSRAM cache miss (random, region over 64 KB) | 172 |
| float divide (ROM `__divsf3`) | 53 | 1/x: table + Newton / recip0.s + 2 Newton | 26 / 22 |
| sqrtf / 1/sqrtf | 56 / 107 | 1/sqrt: bits + 2 Newton / rsqrt0.s + Newton | 35 / 22 |
| sinf / cosf / atan2f / expf / logf / powf | 205 / 187 / 356 / 221 / 197 / 621 | floorf / ceilf / lrintf | 33 / 36 / 64 |
| double add / multiply / divide / sqrt (software) | 69 / 109 / 593 / 808 | int64 multiply / divide | 33 / 115 |
| call of an empty function | 11 | memcpy internal / PSRAM to PSRAM / internal to PSRAM | 0.63 / 8.7 / 3.3 per byte |

Lookup tables: the hardware's reciprocal seed beats the 256-entry table (step 21). The pixel kernel has no
remaining slow operation for a table to replace: its texel address is shifts and masks, the palette is already
a 16-entry table, the depth code is bit arithmetic. The candidates left are caches rather than tables: the
paints (palette times binding colour, about 1 ms a frame per core) could persist across frames, and the
triangle set-up could produce the fixed-point values once instead of on both cores.

Music: the card reader task had priority 4 on core 0, under the game task (5), which yields 1 ms a frame; the
music starved while the race was drawn (heard as choppy). Now priority 8 (it blocks on the card most of the
time) and a 1.5 s buffer: 0 underruns over 60 s of drawn race.

The ULP core cannot help: a few MHz, 8 KB of RTC memory, no access to PSRAM; meant for waking from sleep.

## Music (offline renderer)

`python3 -I tools/esp32/build_music.py "$HOME/Downloads/Daytona_USA_(Sega_Model_2).zip" build/esp-assets/audio`
renders the vgmrips logs through `src/runtime/multipcm.cpp` at the board's 10 MHz / 224 clock (the logs' own
8 MHz header is about 8 cents flat of it), keeps the log's timing, and resamples to 22,050 Hz with a
windowed-sinc low-pass. Gain per track is lowered if the peak would clip. The four race pieces
(t03 to t06) run 215 to 270 s; their loop point is about 7 to 30 s in and the loop runs to the end of the log.
The log carries the chip's sample ROM in its data blocks, so no game ROM is read. Output is under `build/`
(not committed).

## Platform surface the game loop needs (from a grep of `code/` and `include/`)

enDjinn: `enj_state_*` and `enj_mode_*` (frame loop and mode switching), `enj_ctrl_*` (pad state),
`enj_texture_*` and `enj_render_list_add` (render side, replaced by the software renderer), `enj_qfont_*` (text).
KOS: `timer_*`, `sem_*`, `thd_*`, `pvr_get_stats` (the late-frame count), `mm_sbrk`. This is small enough to
shim.

## M2 notes

- Approach (changed from the plan's frame_esp.c idea): compile the DC `practice.c` as it is, with the render,
  audio-engine, loader and front-end calls stubbed in `shim/stubs.c` and a virtual clock in `shim/enj_shim.c`
  (one updater call = one 17,384 us tick). `code/audio/race_sound.c` is compiled for real because its draw hook
  carries a gameplay side effect (`damage.signal` from the wall lane).
- Same flags as the DC build: `-fno-fast-math -ffp-contract=off -fwrapv -fsigned-char -include finite.h`.
  The S3 has fused multiply-add; contraction must stay off.
- Race of 3,764 ticks with the results took 18.45 s on the console including card loads and row printing: about
  4.9 ms per tick, against the 17.4 ms step. The scene lives in PSRAM (`linker.lf`); PSRAM min free 7.17 MB.
- Image with the stub script: 0xb2420 bytes (730 KB) of the 2 MB slot.
- Course 0 packs on the card: `/sdcard/games/daytona/data/{drive.dnd,course0.dnc,course0.dnr,course0-route0..7.dnp,scenery.dnv}`,
  pushed with `racerctl push` (the launcher must be on screen).
- Selecting the game after a flash: the menu starts on the first game; press BOOT (`racerctl key n`) three
  times for slot game3, then PWR.

### M2 gate, Beginner (recorded fast race, automatic transmission, podium and results)

| Run | Ticks | Rows | Differing | Wall time |
| --- | --- | --- | --- | --- |
| Host (`host/Makefile`, gcc) against the recipe's expected rows | 12,477 | 259 | 0 | 0.4 s |
| Console (ESP32-S3), unchanged `practice.c` | 12,477 | 259 | 0 | 53.2 s, 4.3 ms per tick including SD reads and printing |

The script header and the expected rows were made with the recipe's own commands (`trace_race_native ...
--script-header`); the asset recipe runs them again at its `race_script` step, after the slow HUD step.
Image 0xbc400 bytes (770 KB) of the 2 MB slot with that header compiled in. The gate also means the
Xtensa FPU with `-ffp-contract=off` reproduces the arcade arithmetic of the SH-4 build for the whole race.

### M2 gate, all three circuits

| Course | Race | Ticks | Rows | Differing | Console time | PSRAM min free |
| --- | --- | --- | --- | --- | --- | --- |
| Beginner | fast race, 8 laps, podium, results | 12,477 | 259 | 0 | 53.2 s | 7.17 MB |
| Advanced | 3rd place, podium, results | 15,404 | 320 | 0 | 52.0 s | 6.91 MB |
| Expert | 3rd place, podium, results | 16,658 | 345 | 0 | 64.6 s | 6.39 MB |

`host/console_gate.sh COURSE SCRIPT_DIR EXPECTED_ROWS OUT_PREFIX` builds, flashes, runs and diffs one race.
The script headers and expected rows were produced by hand with the recipe's own `trace_race_native` commands
into `build/esp-assets/m2/` while the recipe's slow HUD step ran (Python HUD pack, over 20 minutes); the recipe
makes the same files at its `race_script` and `course*_finish` steps. The recipe needed a newer Pillow than the
system's 10.2 (`Image.get_flattened_data`): a venv with Pillow 13 first on PATH
(`build/esp-assets/venv`), no repository file changed.

Simulation cost is a small part of the frame: 4.3 to 4.5 ms per tick on the console including card reads and
row printing, against the 17.4 ms step.

## Findings that shape M3 and the audio test

- **USB transfer is too slow for the music.** `racerctl push` moved the 1.57 MB `race.pak` in 7 min 6 s
  (about 3.7 KB/s). The 65 MB of music would take hours. The staged card image
  `build/esp-assets/sdcard/games/daytona/` (69 MB: gameplay packs, actor files, music, effects, game.json,
  icon.raw) is meant to be copied onto the card with a card reader.
- **The Dreamcast render path does not fit as it is.** `tools/heap_budget.py` on the recommended set: racing
  uses 11.9 MB (Beginner), 12.5 MB (Advanced), 13.8 MB (Expert) of the Dreamcast's 15.6 MB heap, of which the
  render side holds point caches (0.9 MB world, 1.3 MB cars), prebuilt stream records (1.8 MB), emission
  tables and the packs; the console has about 7 MB of PSRAM free. Keeping `model_submission.h` would
  need most of those tables dropped or rebuilt smaller. The renderer for the console is therefore its own
  (transform, cull, light and rasterise from the DNM6 packs without the Dreamcast stream tables), with
  `tests/stream_sim/sim_main.c` (`colour_frame`, `pvr_texture`, `pvr_wrap`) as the sampling reference.
- Headers the Dreamcast code compiles (KOS `pvr_poly_compile`) are not needed then: the material pack gives
  palette, flips and filter per binding (`nm_material_find`, `nm_material_palette`).

## M3: the software renderer (sw3d)

The Dreamcast scene preparation, `code/render/scene/practice.c`, compiles unchanged against `shim/`. It
computes the camera, the car poses and parts, the cell and object selection, and the projection, and it
fills `scene->world_instances` and `scene->car_instances`. `sw3d/sw3d.c` replaces only the back end
(`nm_render_enqueue_objects`, `nm_sky_enqueue`, `nm_practice_render_load`) and draws straight into a
picture with a 1/z depth buffer. The rules it follows are listed at the top of the file.

Host frames: `host/Makefile` target `render_host`, run as
`render_host build/esp-assets/out/assets-ssfar4f/ OUT_PREFIX FRAME...`. It writes PPM pictures of the
scripted Beginner race.

| Host frame (x86, one core, -O2) | World | Cars | Sky |
| --- | --- | --- | --- |
| 3000 (start of the race) | 4.6 ms | 2.3 ms | 0.3 ms |
| 6000 | 3.9 ms | 1.2 ms | 0.2 ms |

Version 1 leaves these out:
- mip levels (base level only);
- bilinear filtering;
- translucent smoke;
- shadows;
- the stipple catch fence;
- level-of-detail beyond level 0;
- live specular;
- the HUD and the 2D screens.

The picture is the 496x384 logical view stretched to 284x240, which is about 9% taller than true.

## The card (complete as of this copy)

`build/esp-assets/sdcard/games/daytona/` (86 MB, 342 files) was copied to the card and checked with
`diff -rq`. It holds:
- the whole recommended asset set (`assets-ssfar4f`): gameplay packs, car and world packs, materials and
  skies for the three courses, and the HUD, label, result, podium, finish, select, name and attract-2D
  files the front end will need;
- `music/`: 45 WAVs and `tracks.tsv`;
- `sfx/`: `race.pak`, `voices.pak` and `gains.bin`, plus `engine.pak` (`tools/esp32/build_engine_pack.py`:
  the ten engine, slip and skid layers' looping samples with rate and gain per controller row, from
  `prepare_sfx render` and the sound program's `update_engines` tables).

Nothing planned for M4 to M7 needs another file on the card. A change of format would; then a fast binary
upload over USB inside the game is the way to avoid moving the card again.

### Where M4 stands (this session)

13 to 25 pictures/s on the Beginner scripted race, about 17 on average, against the 30 target. A frame is
the simulation's ticks (about 4 ms each, 2 to 4 per picture at this rate), the triangle list on both cores
(7-20 ms) and the fill on both cores (20-41 ms), one after the other. Ideas not yet tried, largest first:
- per-row overhead: rows are short (thousands of small triangles); step the edges and attribute starts
  incrementally instead of recomputing them from the plane each row;
- front-to-back coverage per row (OpenMotoRacer's free-interval lists): no depth reads for covered pixels
  and no overdraw; needs care with the road-surface depth class and decals;
- overlapping the simulation of the next picture with the fill of this one (one frame of latency);
- the ESP32-S3's SIMD (PIE): fits clears, copies and fills; the textured pixel loop is a per-pixel palette
  gather, which the vector unit cannot do (no per-lane table lookup), so it is not the lever there;
- fewer pixels or less detail on the heaviest views (draw distance), if the above is not enough.
PSRAM is nearly full on Beginner (list caps 2 x 2700 triangles); Expert's larger world pack will need the
compact mesh format of the plan before it can run.
