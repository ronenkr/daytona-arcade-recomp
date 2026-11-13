# The native game's gameplay sources are compiled from platform/dreamcast-native by path (no copies),
# against shim/ in place of KOS, enDjinn and SH4ZAM. ROM-derived generated headers come from
# DAYTONA_HEADERS (the recipe's headers/ directory under build/); they never enter the repository.
get_filename_component(REPO ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(DN ${REPO}/platform/dreamcast-native)
set(SHIM ${CMAKE_CURRENT_LIST_DIR}/../shim)
if(DEFINED ENV{DAYTONA_HEADERS})
  set(HEADERS $ENV{DAYTONA_HEADERS})
else()
  set(HEADERS ${REPO}/build/esp-assets/out/headers)
endif()
# The race script header (race_script_data.h): script-fast (automatic), script-manual, or any directory.
if(DEFINED ENV{DAYTONA_SCRIPT_DIR})
  set(SCRIPT_DIR $ENV{DAYTONA_SCRIPT_DIR})
else()
  set(SCRIPT_DIR ${HEADERS}/script-fast)
endif()

set(GAMEPLAY_SRCS
    ${DN}/code/practice.c ${DN}/code/gameplay/results.c ${DN}/code/drive.c ${DN}/code/contact.c
    ${DN}/code/road.c ${DN}/code/path.c ${DN}/code/visibility.c ${DN}/code/props.c
    ${DN}/code/props_scene.c ${DN}/code/expert_actors.c ${DN}/code/geometry.c
    ${DN}/code/audio/race_sound.c ${DN}/code/save/game_save.c ${DN}/code/save/save_data.c
    ${DN}/code/save/save_store.c)

# DAYTONA_MODE: m2 (headless scripted race, default), audio (sound test), render (the scripted race drawn by
# sw3d in real time, with sound).
set(RENDER_SRCS ${DN}/code/render/scene/practice.c ${DN}/code/meshpack.c ${DN}/code/material_pack.c
    ${DN}/code/render/scene/track_surface.c ${DN}/code/render/scene/damage_bodies.c
    ${DN}/code/render/models/scenery_camera.c ${CMAKE_CURRENT_LIST_DIR}/../sw3d/sw3d.c)
# game: the whole game (attract, title, select, race, results, name entry) played with tilt and the buttons.
if("$ENV{DAYTONA_MODE}" STREQUAL "game")
  list(APPEND GAMEPLAY_SRCS ${DN}/code/gameplay/title_mode.c ${DN}/code/gameplay/select_mode.c
       ${DN}/code/gameplay/settings_mode.c ${DN}/code/gameplay/name_mode.c ${DN}/code/gameplay/name_scene.c)
  set(UI_SRCS ${DN}/code/render/ui/title_render.c ${DN}/code/render/ui/select_render.c ${DN}/code/render/ui/hud_render.c
      ${DN}/code/render/ui/attract_2d_render.c ${DN}/code/render/ui/result_labels.c ${DN}/code/render/ui/result_render.c
      ${DN}/code/render/ui/name_labels.c ${DN}/code/render/ui/name_render.c ${DN}/code/render/ui/podium_render.c
      ${DN}/code/render/ui/finish_render.c ${DN}/code/render/materials/palette_ram.c
      ${DN}/code/render/materials/result_materials.c)
  set(MODE_SRCS ${RENDER_SRCS} render_main.c console_esp.c save_sd.c ${UI_SRCS} ${SHIM}/pvr_draw.c
      ${SHIM}/frame_ui.c)
  set(WRAP_SCENE_SETUP 1)
elseif("$ENV{DAYTONA_MODE}" STREQUAL "render")
  set(MODE_SRCS ${RENDER_SRCS} render_main.c)
  set(WRAP_SCENE_SETUP 1)
elseif("$ENV{DAYTONA_MODE}" STREQUAL "bench")
  # bench: instruction, library and memory timings, and the span kernel, printed on the serial port
  set(MODE_SRCS ${RENDER_SRCS} render_main.c bench.c bench_ops.S)
  set(WRAP_SCENE_SETUP 1)
else()
  set(MODE_SRCS ${SHIM}/stubs_render.c)
endif()
idf_component_register(
    SRCS main.c perf.c audio_esp.c ${SHIM}/enj_shim.c ${SHIM}/pvr_lite.c ${SHIM}/stubs.c ${SHIM}/stubs_ui.c m2_race.c ${GAMEPLAY_SRCS} ${MODE_SRCS}
    INCLUDE_DIRS .
    LDFRAGMENTS linker.lf
    PRIV_REQUIRES racer_board esp_timer esp_psram esp_hw_support heap esp_driver_usb_serial_jtag esp_partition)

# Gameplay must be bit-exact to the arcade: no fused multiply-add (Xtensa has madd.s), no fast math.
set(GAMEPLAY_FLAGS -fno-fast-math -ffp-contract=off -fwrapv -fsigned-char -include ${DN}/include/finite.h)
foreach(f ${GAMEPLAY_SRCS})
  set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "${GAMEPLAY_FLAGS}")
endforeach()
# The course the scripted race is on (0 Beginner, 1 Expert, 2 Advanced), matching the script header.
if(DEFINED ENV{DAYTONA_COURSE} AND NOT "$ENV{DAYTONA_COURSE}" STREQUAL "0")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE NM_COURSE=$ENV{DAYTONA_COURSE})
endif()
# DAYTONA_MODE: m2 (headless scripted race, default), audio (sound test); the shim's audio stubs go away
# with DAYTONA_AUDIO, which the audio build and the full game define.
if("$ENV{DAYTONA_MODE}" STREQUAL "audio")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_AUDIO DAYTONA_AUDIO_TEST)
elseif("$ENV{DAYTONA_MODE}" STREQUAL "game")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_AUDIO DAYTONA_RENDER DAYTONA_REALTIME DAYTONA_GAME)
elseif("$ENV{DAYTONA_MODE}" STREQUAL "render")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_AUDIO DAYTONA_RENDER DAYTONA_REALTIME)
elseif("$ENV{DAYTONA_MODE}" STREQUAL "bench")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_AUDIO DAYTONA_REALTIME DAYTONA_BENCH SW3D_BENCH)
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_M2)
endif()
# The renderer is presentation only: fast math lets the divides use the FPU's reciprocal (no ROM __divsf3).
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../sw3d/sw3d.c PROPERTIES COMPILE_OPTIONS "-ffast-math")
set_source_files_properties(${SHIM}/pvr_draw.c PROPERTIES COMPILE_OPTIONS "-ffast-math")
# DAYTONA_DEFS: extra definitions for measurement builds, e.g. "SW3D_PROFILE;SW3D_FAKE_TEX".
if(DEFINED ENV{DAYTONA_DEFS} AND NOT "$ENV{DAYTONA_DEFS}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE $ENV{DAYTONA_DEFS})
endif()
if("$ENV{DAYTONA_MODE}" STREQUAL "game")
  # the Dreamcast game build's HUD settings (local.cfg.mk defaults) and the 2D screens' generated headers
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DAYTONA_RACE_TICK_LIMIT=0 DAYTONA_UI NATIVE_HUD NM_HUD_RADAR_PANEL=1
      NM_HUD_GEAR_GATE=1 NM_HUD_GEAR_DIGIT=1 NATIVE_HUD_STRIPS)
  target_include_directories(${COMPONENT_LIB} PRIVATE ${HEADERS}/title ${HEADERS}/hud ${HEADERS}/attract-2d
      ${HEADERS}/podium-screen ${HEADERS}/result-labels ${HEADERS}/result-pal4 ${HEADERS}/name/labels)
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nm_hud_render_tick")
else()   # the recorded race of the recipe's script header, straight from power-on
  target_compile_definitions(${COMPONENT_LIB} PRIVATE NATIVE_RACE_SCRIPT NATIVE_RACE_CHECKPOINTS DAYTONA_AUTOSTART
      DAYTONA_RACE_TICK_LIMIT=0)
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE ${SHIM}/include ${CMAKE_CURRENT_LIST_DIR}/../sw3d ${DN}/include
    ${HEADERS}/race-header ${HEADERS}/intent-data ${HEADERS}/race-start-data ${HEADERS}/record-data
    ${HEADERS}/podium-data ${HEADERS}/result-data ${HEADERS}/name ${HEADERS}/slot-glass ${SCRIPT_DIR})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)

# A measurement switch: DAYTONA_SW3D_DEFS="SW3D_NO_TEX" and the like.
if(DEFINED ENV{DAYTONA_SW3D_DEFS})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE $ENV{DAYTONA_SW3D_DEFS}) # SW3D_EXPERIMENT
endif()

# Times the Dreamcast scene set-up (render_main.c wraps its entry point).
if(WRAP_SCENE_SETUP)
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nm_practice_render_enqueue")
endif()
# Big reads in pieces, so that the music keeps streaming during course loads (main/audio_esp.c).
if(NOT "$ENV{DAYTONA_MODE}" STREQUAL "" AND NOT "$ENV{DAYTONA_MODE}" STREQUAL "m2")
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=fread")
endif()
