# Vehicular Mesh — SDL simulator build.
# `make deps` fetches LVGL + cJSON; `make -j8` builds; `make run` runs.

CC := clang
BUILD := build
BIN := $(BUILD)/vmesh-sim

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

# Match the installed SDL2's architecture (Intel-prefix Homebrew ships
# x86_64-only; the sim then runs under Rosetta — fine for a dev tool).
SDL_LIBDIR := $(shell pkg-config --variable=libdir sdl2)
ARCH := $(shell lipo -archs $(SDL_LIBDIR)/libSDL2.dylib 2>/dev/null | grep -qw arm64 && echo arm64 || echo x86_64)

CFLAGS := -std=c11 -arch $(ARCH) -O2 -g -Wall \
	-I. -Imsg -Imesh -Inet -Iui -Isim -Ithird_party -Ithird_party/lvgl \
	-DLV_CONF_INCLUDE_SIMPLE \
	$(SDL_CFLAGS)

APP_SRCS := \
	ui/ui.c ui/map_view.c ui/hazard_store.c ui/settings.c ui/names.c \
	ui/town_square.c \
	msg/vmesh_wire.c mesh/vmesh_mesh.c net/lora_dtu.c net/provision.c \
	targets/sdl/serial_radio.c targets/sdl/fake_provision.c \
	sim/scenario.c \
	targets/sdl/main.c \
	third_party/cJSON/cJSON.c \
	third_party/lodepng/lodepng.c

LVGL_SRCS := $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS) $(LVGL_SRCS))

.PHONY: all run deps clean test meshsim

meshsim:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Imsg -Imesh -o $(BUILD)/meshsim \
		tools/meshsim/meshsim.c mesh/vmesh_mesh.c -lm
	./$(BUILD)/meshsim
.DEFAULT_GOAL := all   # `test:` is declared first; default stays the app

test:
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O1 -Wall -Imsg -o $(BUILD)/test_wire \
		tests/test_wire.c msg/vmesh_wire.c
	./$(BUILD)/test_wire
	$(CC) -std=c11 -O1 -Wall -Imsg -Inet -o $(BUILD)/test_net \
		tests/test_net.c net/nmea.c net/lora_dtu.c msg/vmesh_wire.c
	./$(BUILD)/test_net
	$(CC) -std=c11 -O1 -Wall -Imsg -Imesh -o $(BUILD)/test_mesh \
		tests/test_mesh.c mesh/vmesh_mesh.c
	./$(BUILD)/test_mesh

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -arch $(ARCH) $(OBJS) $(SDL_LIBS) -lm -o $@

composer: $(BUILD)/waycast-composer
$(BUILD)/waycast-composer: tools/composer/composer.c net/lora_dtu.c msg/vmesh_wire.c
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Imsg -Inet -o $@ $^ -lpthread

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) sim/scenarios/highway_demo.json

deps:
	test -d third_party/lvgl || git clone --depth 1 --branch v9.2.2 \
		https://github.com/lvgl/lvgl.git third_party/lvgl
	test -f third_party/cJSON/cJSON.c || ( mkdir -p third_party/cJSON && \
		curl -sfL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c \
			-o third_party/cJSON/cJSON.c && \
		curl -sfL https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h \
			-o third_party/cJSON/cJSON.h )

clean:
	rm -rf $(BUILD)
