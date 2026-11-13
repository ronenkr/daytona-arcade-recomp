# Host builds of the native game against shim/ (no KOS, no enDjinn, no PVR):
#   headless     the scripted race without graphics; prints the CHECKPOINT rows the console must equal
#   render_host  the same race with the EspRacer software renderer (sw3d/), writes frames as PPM
# The ESP-IDF build in ../ compiles the same files with the same flags.
#
#   make [COURSE=n] [HEADERS=...] [SCRIPT_DIR=...] [OUT=build] [EXTRA_CFLAGS=...]
#   build/headless ASSET_DIR
#   build/render_host ASSET_DIR FRAME_TICK OUT.ppm
DN      := ../../dreamcast-native
HEADERS ?= ../../../build/esp-assets/out/headers
SCRIPT_DIR ?= $(HEADERS)/script-fast
OUT     ?= build
CC      ?= gcc
COURSE  ?= 0
GAMEPLAY_FLAGS := -std=gnu17 -O2 -fno-fast-math -ffp-contract=off -fwrapv -fsigned-char -include $(DN)/include/finite.h
# GAME=1: the whole game (attract, title, select, race, results, name entry) with the pad from DAYTONA_INPUT
# (shim/enj_shim.c) instead of the recorded race; use its own OUT directory.
GAME    ?= 0
ifeq ($(GAME),1)
DEFS    := -DDAYTONA_GAME -DDAYTONA_RACE_TICK_LIMIT=0 -DDAYTONA_UI -DNATIVE_HUD -DNM_HUD_RADAR_PANEL=1 \
           -DNM_HUD_GEAR_GATE=1 -DNM_HUD_GEAR_DIGIT=1 -DNATIVE_HUD_STRIPS
else
DEFS    := $(if $(filter-out 0,$(COURSE)),-DNM_COURSE=$(COURSE)) -DNATIVE_RACE_SCRIPT -DNATIVE_RACE_CHECKPOINTS -DDAYTONA_AUTOSTART -DDAYTONA_RACE_TICK_LIMIT=0
endif
INCS    := -I../shim/include -I$(DN)/include -I../sw3d $(addprefix -I$(HEADERS)/,race-header intent-data race-start-data record-data podium-data result-data name slot-glass) -I$(SCRIPT_DIR)
CFLAGS   = $(GAMEPLAY_FLAGS) $(DEFS) $(INCS) $(GAME_INCS) -w $(EXTRA_CFLAGS)

COMMON  := $(DN)/code/practice.c $(DN)/code/gameplay/results.c $(DN)/code/drive.c $(DN)/code/contact.c \
           $(DN)/code/road.c $(DN)/code/path.c $(DN)/code/visibility.c $(DN)/code/props.c \
           $(DN)/code/props_scene.c $(DN)/code/expert_actors.c $(DN)/code/geometry.c \
           $(DN)/code/audio/race_sound.c $(DN)/code/save/game_save.c $(DN)/code/save/save_data.c \
           $(DN)/code/save/save_store.c ../shim/enj_shim.c ../shim/stubs.c ../shim/stubs_ui.c ../shim/pvr_lite.c
ifeq ($(GAME),1)
COMMON  += $(DN)/code/gameplay/title_mode.c $(DN)/code/gameplay/select_mode.c $(DN)/code/gameplay/settings_mode.c \
           $(DN)/code/gameplay/name_mode.c $(DN)/code/gameplay/name_scene.c ../shim/pvr_draw.c \
           $(addprefix $(DN)/code/render/ui/,title_render.c select_render.c hud_render.c attract_2d_render.c \
             result_labels.c result_render.c name_labels.c name_render.c podium_render.c finish_render.c) \
           $(DN)/code/render/materials/palette_ram.c $(DN)/code/render/materials/result_materials.c
GAME_RENDER := ../shim/frame_ui.c
GAME_INCS := -I$(OUT)/title $(addprefix -I$(HEADERS)/,hud attract-2d podium-screen result-labels result-pal4 name/labels)
# the title's generated data uses #embed, which this gcc lacks: expanded into $(OUT)/title
$(OUT)/title/native_scene.inc: $(HEADERS)/title/native_scene.inc expand_embed.py | $(OUT)
	mkdir -p $(OUT)/title && python3 -I expand_embed.py $< $@
$(OUT)/$(subst /,_,$(subst ../,,$(DN:../%=%)/code/render/ui/title_render.o)): $(OUT)/title/native_scene.inc
LDEXTRA += -Wl,--wrap=nm_hud_render_tick
endif
RENDER  := $(DN)/code/render/scene/practice.c $(DN)/code/meshpack.c $(DN)/code/material_pack.c \
           $(DN)/code/render/scene/track_surface.c $(DN)/code/render/scene/damage_bodies.c \
           $(DN)/code/render/models/scenery_camera.c ../sw3d/sw3d.c main_render.c $(GAME_RENDER)
HEADLESS := ../shim/stubs_render.c main_headless.c

# objects get unique names: render/scene/practice.c and code/practice.c share a basename
obj = $(OUT)/$(subst /,_,$(subst ../,,$(1:.c=.o)))
all: $(OUT)/headless
$(OUT)/headless: $(foreach f,$(COMMON) $(HEADLESS),$(call obj,$(f)))
	$(CC) -o $@ $^ -lm $(LDEXTRA)
$(OUT)/render_host: $(foreach f,$(COMMON) $(RENDER),$(call obj,$(f)))
	$(CC) -o $@ $^ -lm -lz $(LDEXTRA)
define rule
$(call obj,$(1)): $(1) | $(OUT)
	$(CC) $(CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(COMMON) $(HEADLESS) $(RENDER),$(eval $(call rule,$(f))))
$(OUT):
	mkdir -p $(OUT)
clean:
	rm -rf $(OUT)
