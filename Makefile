CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g

CORE = src/core/vm.c src/core/sha256.c src/core/tasks.c src/core/font.c src/core/shell.c
HDRS = $(wildcard src/core/*.h)

all: build/mk1 build/test

build/mk1: $(CORE) src/core/content_gen.c src/host/main.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c src/host/main.c

build/test: $(CORE) src/core/content_gen.c tests/test_core.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/core/content_gen.c tests/test_core.c

build/mkcontent: tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ tools/mkcontent.c src/core/vm.c src/core/sha256.c src/core/tasks.c

# needs the private content/ folder; the generated file is committed, so a public checkout builds without it
content: build/mkcontent
	build/mkcontent content src/core/content_gen.c

test: build/test
	build/test content

run: build/mk1
	build/mk1

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all content test run clean
