CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g

CORE = src/core/vm.c src/core/sha256.c src/core/tasks.c src/core/token.c src/core/font.c src/core/shell.c
HDRS = $(wildcard src/core/*.h)
# the card's screen, light, panel and flash store: no hardware, so they build here too. The card's own screens
# are src/card/own.c; the demo firmware has demo/own.c in their place
CARD = src/card/face.c src/card/prompt.c src/card/cube.c src/card/saver.c src/card/update.c src/card/light.c src/card/panel.c src/card/store.c
CARD_HDRS = $(wildcard src/card/*.h)
DEMO = demo/own.c demo/pong.c
DEMO_HDRS = $(wildcard demo/*.h)
CARD_CMAKE = cmake -S src/card -B build/card -G Ninja -DCMAKE_BUILD_TYPE=Release -DMK1_RELEASE_VERSION=$(MK1_RELEASE_VERSION) $(if $(PICOTOOL_DIR),-Dpicotool_DIR=$(PICOTOOL_DIR))

all: build/mk1 build/test build/test-card build/test-demo

build/mk1: $(CORE) src/core/content_gen.c src/host/main.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c src/host/main.c

build/test: $(CORE) src/core/content_gen.c tests/test_core.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c tests/test_core.c

build/test-card: $(CARD) src/card/own.c src/core/font.c tests/test_card.c $(CARD_HDRS) $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CARD) src/card/own.c src/core/font.c tests/test_card.c -lm

build/test-demo: $(CARD) $(DEMO) src/core/font.c demo/test_demo.c $(CARD_HDRS) $(DEMO_HDRS) $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CARD) $(DEMO) src/core/font.c demo/test_demo.c -lm

build/cardsim: $(CORE) src/core/content_gen.c $(CARD) src/card/own.c tools/cardsim.c tools/visit.c tools/cardsim.h $(HDRS) $(CARD_HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c $(CARD) src/card/own.c tools/cardsim.c tools/visit.c -lm

build/cardsim-demo: $(CORE) src/core/content_gen.c $(CARD) $(DEMO) tools/cardsim.c demo/visit.c tools/cardsim.h $(HDRS) $(CARD_HDRS) $(DEMO_HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c $(CARD) $(DEMO) tools/cardsim.c demo/visit.c -lm

build/mkcontent: tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c

# needs the private content/ folder; the generated file is committed, so a public checkout builds without it
content: build/mkcontent
	build/mkcontent content src/core/content_gen.c

test: build/test build/test-card build/test-demo
	build/test content
	build/test-card
	build/test-demo

# the card's screens as a video: boot, the prompt, a terminal, sudo, ^D, the cube, sleep (needs ffmpeg)
preview: build/cardsim
	build/cardsim | ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - -pix_fmt yuv420p build/card-preview.mp4

# what the matrix loops while the card takes a new firmware from the page
preview-update: build/cardsim
	build/cardsim update | ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - -pix_fmt yuv420p build/update-preview.mp4

# every saver the card picks from when left alone
preview-savers: build/cardsim
	build/cardsim savers | ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - -pix_fmt yuv420p build/saver-preview.mp4

# the demo's, with pong and a hold to the cube
preview-demo: build/cardsim-demo
	build/cardsim-demo | ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - -pix_fmt yuv420p build/demo-preview.mp4

# the firmware, build/card/mk1.uf2: needs PICO_SDK_PATH, PICO_TOOLCHAIN_PATH and picotool (README)
card:
	$(CARD_CMAKE)
	cmake --build build/card --target mk1

# the demo firmware, build/card/mk1-demo.uf2
demo:
	$(CARD_CMAKE)
	cmake --build build/card --target mk1-demo

run: build/mk1
	build/mk1

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all content test preview preview-demo preview-update preview-savers card demo run clean
