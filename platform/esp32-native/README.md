# Daytona USA for EspRacer (ESP32-S3)

The native game (`platform/dreamcast-native`) on the EspRacer console, an ESP32-S3 with two 240 MHz cores,
8 MB of PSRAM, a 284x240 screen, a tilt sensor and two buttons. It is the whole game: attract mode, title,
car select, the three circuits (Beginner, Advanced, Expert), results, name entry and records, with music,
effects and the announcer. It is made from `daytona93`; the ROMs are not in this repository.

## How it plays

| Input | In a race | In the menus |
| --- | --- | --- |
| Tilt the console | steer | |
| BOOT button | accelerate | start / select |
| PWR button | brake | |
| Touch | view change, gears | start / select |

BOOT + PWR held for 2 s returns to the EspRacer menu. Every race starts on the follow camera. The text is
the Japanese setting of the game (the factory COUNTRY value).

## How it differs from the Dreamcast port

- **The gameplay code is the same code.** `code/*.c` and `include/` of `dreamcast-native` are compiled by path,
  with `-ffp-contract=off` so that the Xtensa fused multiply-add cannot change a result. All three circuits give
  identical checkpoint rows on the console (259 / 320 / 345), as on the Dreamcast.
- **The renderer is new** (`sw3d/`): the chip has no GPU, so a software rasteriser draws the textured, lit
  triangles of the race, split between the two cores, with the sky and the 2D screens. The Dreamcast's 2D code
  runs unchanged: its PVR records are captured (`shim/pvr_lite.c`) and drawn in software (`shim/pvr_draw.c`).
- **Memory:** the world and car textures live in a flash data partition (`daytona`), copied from the SD card
  at the first start and read through the memory map. Meshes are read from the card a table at a time.
- **Sound:** the music is the game's own MultiPCM log rendered offline to 22,050 Hz WAV
  (`tools/esp32/vgm_render.cpp`), streamed from the card with loop points; effects, the announcer and the
  engine layers are mixed on the console (`main/audio_esp.c`). The default volume is 20%.

## Speed

About 21 pictures/s on average and 16 to 18 in the heaviest scenes, at full picture quality, with the
simulation at its real 57.5 Hz step. The 30 pictures/s target of the porter guide was waived by the owner.
The tuning log, with the rejected attempts, is in [NOTES.md](NOTES.md).

## Building and running

1. Put the ROM set in `roms/daytona93.zip` (merged with its parent set, see NOTES.md, "ROM set") and build the
   assets with the repository's recipe; the converters for this port are in `tools/esp32/`.
   The result (86 MB) is copied to the SD card as `/games/daytona/`.
2. Build and flash from the EspRacer repository, which owns the partition table (`partitions.csv` here is a
   link to it):

   ```
   . ~/esp/v6.0/esp-idf/export.sh
   cd ../../../EspRacer
   tools/build_all.sh daytona
   tools/flash_all.sh daytona
   ```

   `game.json` and `icon.raw` for the menu are in EspRacer's `sdcard_template/games/daytona/`.
3. The first start copies the textures into the `daytona` data partition (never move or resize it).

Host tools (a picture renderer and the checkpoint gate) are in `host/`, and `host/console_watch.py` logs the
console's USB serial and takes screenshots while keys are sent.

Third-party code used here is listed in [THIRD_PARTY.md](THIRD_PARTY.md).
