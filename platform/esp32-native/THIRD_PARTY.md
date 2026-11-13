# Third-party components of the EspRacer port

The port's own code is under the repository's BSD-3-Clause licence. What it takes from or links with others:

| Component | Upstream | Commit / version | Licence | Use | Changes |
| --- | --- | --- | --- | --- | --- |
| KallistiOS `kernel/arch/dreamcast/hardware/pvr/pvr_prim.c`, `include/dc/pvr.h` | https://github.com/KallistiOS/KallistiOS | `368a2eb9584a7d130a6e77e299468c0b64fa1a9f` (the tree the Dreamcast port uses) | KOS License (BSD-3-Clause) | **Code adapted** into `shim/pvr_lite.c` (`pvr_poly_cxt_*`, `pvr_poly_compile`, `pvr_sprite_*`) and **constants and structures** into `shim/include/enDjinn/pvr_lite.h` (the TA header fields, `pvr_poly_hdr_t`, `pvr_vertex_t`), so the captured records are the PVR's own, bit for bit. Notice in both files. | Texture addresses are keys of a registry over ordinary memory; palette RAM is a table; lists are captured, not sent to hardware. |
| KallistiOS `utils/pvrtex/file_dctex.h` (DcTx header layout) | same | same | pvrtex: LGPL as a whole; non-FFmpeg files may be used as public domain/BSD/MIT (its README) | Read: the header fields and the VQ small-codebook rule in `pvr_lite.h` (`fDtHeader`, `fDtAdjustPVRPointer`) follow it. No code copied. | — |
| enDjinn texture loader (`enj_texture_load_blob/file`) | https://github.com/dfchil/enDjinn | `725f8b192c18ba56298eca1b5eaf8abc216e67be` | MIT | Interface reimplemented in `shim/pvr_lite.c` (same record, `enj_texture_info_t`) so the Dreamcast port's render/ui code runs unchanged; the other `shim/include/enDjinn/` headers declare the subset of enDjinn's API that code calls. | Loads into PSRAM, or uses the copy in the flash store. |
| ESP-IDF | https://github.com/espressif/esp-idf | v6.0 | Apache-2.0 | The console's SDK (FreeRTOS, drivers, FATFS, esp_partition). Installed by the user, not fetched by this tree. | None. |
| espressif/esp_codec_dev | https://components.espressif.com/components/espressif/esp_codec_dev | 3.0.0 (`dependencies.lock`) | Apache-2.0 | ES8311 codec driver, required by EspRacer's `racer_board` component; fetched into git-ignored `managed_components/` by the IDF component manager. | None. |
| EspRacer `components/racer_board` | the console owner's EspRacer tree (`ESPRACER_DIR`, default `../../../EspRacer`) | — | the owner's | Board support (LCD, audio, IMU, touch, buttons, SD, menu return); `partitions.csv` is a link to its partition table. | None. |

The music and effects come from the user's own copy of vgmrips' Daytona USA pack, rendered by `tools/esp32/vgm_render.cpp` through this repository's MultiPCM (`src/runtime/multipcm.cpp`, recorded in the root `THIRD_PARTY.md`); the output stays in git-ignored `build/`.
