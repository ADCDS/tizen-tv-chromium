# mgl: GLES2 remoting so Chromium (Debian armhf, in the chroot) can draw with the
# TV's Mali GPU. Needs zig (tested with 0.17.1) as the cross compiler.
#   make            client + TV server   (run ./fetch-tv-libs.sh once first)
#   make gen        regenerate the mechanical encoders/decoders from the GLES2 headers
#   make pc         server + relaytest for a PC with Mesa (development)
#   make glprobe    native GPU probe/benchmark for the TV
ZIG ?= zig
# The UN43MU6100's SoC. Other Tizen 3.0 sets may need a different -mcpu.
TV_CPU ?= cortex_a72
WARN = -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
INC = -Isrc -Ithird_party/include

# TV userspace: soft-float EABI, glibc 2.20. The 2014 loader segfaults on
# BIND_NOW, so -z lazy is required; the TV's libraries are linked from sysroot/.
TV_FLAGS = -target arm-linux-gnueabi.2.20 -mcpu=$(TV_CPU) -O2 -fPIE -pie -Wl,-z,lazy -s \
	-DWL_EGL_PLATFORM -Lsysroot -Wl,--allow-shlib-undefined
TV_LIBS = -lwayland-client -lwayland-egl -lEGL -lGLESv2 -ldl
# Chroot side: Debian trixie armhf (hard-float).
ARMHF_FLAGS = -target arm-linux-gnueabihf.2.31 -mcpu=$(TV_CPU) -O2 -shared -fPIC -s

CLIENT_SRC = src/client/core.c src/client/egl.c src/client/gl_gen.c src/client/gl_special.c src/client/arrays.c
SERVER_SRC = src/server/main.c src/server/gl_gen.c src/server/gl_special.c
HEADERS = $(wildcard src/*.h src/*/*.h)

all: build/armhf/libEGL.so.1 build/tv/mgl-server

build/armhf/libEGL.so.1: $(CLIENT_SRC) $(HEADERS)
	@mkdir -p $(@D)
	$(ZIG) cc $(ARMHF_FLAGS) $(WARN) $(INC) $(CLIENT_SRC) -ldl -lpthread -Wl,-soname,libEGL.so.1 -o $@

build/tv/mgl-server: $(SERVER_SRC) src/server/plat_wl.c $(HEADERS) sysroot/libEGL.so
	@mkdir -p $(@D)
	$(ZIG) cc $(TV_FLAGS) $(WARN) $(INC) $(SERVER_SRC) src/server/plat_wl.c $(TV_LIBS) -o $@

build/tv/glprobe: tools/glprobe.c sysroot/libEGL.so
	@mkdir -p $(@D)
	$(ZIG) cc $(TV_FLAGS) $(INC) $< $(TV_LIBS) -lm -o $@

sysroot/libEGL.so:
	@echo "sysroot/ is empty: run ./fetch-tv-libs.sh (copies the TV's GL libraries)" >&2; exit 1

gen:
	@mkdir -p build/gen
	python3 gen/gen_remote.py build/gen third_party/include/GLES2/gl2.h third_party/include/GLES2/gl2ext.h
	mv build/gen/gl_ops.h src/gl_ops.h
	mv build/gen/client_gl_gen.c src/client/gl_gen.c
	mv build/gen/server_gl_gen.c src/server/gl_gen.c

# Development on a PC: the server on the host's Mesa (window surfaces become
# pbuffers), the client natively, and a relay self-test.
pc: build/pc/mgl-server build/pc/libEGL.so.1 build/pc/relaytest

build/pc/mgl-server: $(SERVER_SRC) src/server/plat_pc.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) -O2 $(WARN) $(INC) $(SERVER_SRC) src/server/plat_pc.c -lEGL -lGLESv2 -ldl -o $@

build/pc/libEGL.so.1: $(CLIENT_SRC) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) -O2 -shared -fPIC $(WARN) $(INC) $(CLIENT_SRC) -ldl -lpthread -Wl,-soname,libEGL.so.1 -o $@

build/pc/relaytest: test/relaytest.c build/pc/libEGL.so.1
	$(CC) -O2 -Wall $(INC) $< -Lbuild/pc -l:libEGL.so.1 -Wl,-rpath,'$$ORIGIN' -o $@

glprobe: build/tv/glprobe

clean:
	rm -rf build

.PHONY: all gen pc glprobe clean
