CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g

CORE = src/core/vm.c src/core/sha256.c src/core/tasks.c src/core/font.c src/core/shell.c
HDRS = $(wildcard src/core/*.h)
# the card's own screens, game, light, panel and flash store: no hardware, so they build here too
CARD = src/card/face.c src/card/pong.c src/card/cube.c src/card/light.c src/card/panel.c src/card/store.c
CARD_HDRS = $(wildcard src/card/*.h)

all: build/mk1 build/test build/test-card

build/mk1: $(CORE) src/core/content_gen.c src/host/main.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c src/host/main.c

build/test: $(CORE) src/core/content_gen.c tests/test_core.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c tests/test_core.c

build/test-card: $(CARD) src/core/font.c tests/test_card.c $(CARD_HDRS) $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CARD) src/core/font.c tests/test_card.c -lm

build/cardsim: $(CORE) src/core/content_gen.c $(CARD) tools/cardsim.c $(HDRS) $(CARD_HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c $(CARD) tools/cardsim.c -lm

build/mkcontent: tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c

# needs the private content/ folder; the generated file is committed, so a public checkout builds without it
content: build/mkcontent
	build/mkcontent content src/core/content_gen.c

test: build/test build/test-card
	build/test content
	build/test-card

# the card's screens as a video: boot, idle, pong, the cube, a terminal, sudo, ^D (needs ffmpeg)
preview: build/cardsim
	build/cardsim | ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - -pix_fmt yuv420p build/card-preview.mp4

# the firmware, build/card/mk1.uf2: needs PICO_SDK_PATH, PICO_TOOLCHAIN_PATH and picotool (README)
card:
	cmake -S src/card -B build/card -G Ninja -DCMAKE_BUILD_TYPE=Release $(if $(PICOTOOL_DIR),-Dpicotool_DIR=$(PICOTOOL_DIR))
	cmake --build build/card

run: build/mk1
	build/mk1

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all content test preview card run clean
