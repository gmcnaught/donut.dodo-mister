# Cross-build of the MiSTer GL glue module (armhf, Cortex-A9).
#
# Produces libmisterglue.so: the Godot GLES2 state shadow + draw decode, sitting
# on gmloader-next's RasterBackend seam with the mfgpu (FPGA fabric) back-end
# behind it. Our patched SDL2 dlopens this and routes GL through it.

CROSS   ?= arm-linux-gnueabihf-
CXX      = $(CROSS)g++
CC       = $(CROSS)gcc

SRCDIR   = src
VENDOR   = $(SRCDIR)/vendor
OUT     ?= build

# The fabric's framebuffer geometry is a wire contract shared with the RBF, the
# refmodel (src/vendor/mfgpu/refmodel/blitter_ref.h) and the RTL
# (blitter_defs.vh). Donut Dodo's Godot viewport is a fixed 320x240, which is
# what the donutdodo/fb-320x240 core is built for.
FB_W    ?= 320
FB_H    ?= 240

ARCHFLAGS = -march=armv7-a -mfpu=neon -mtune=cortex-a9 -mfloat-abi=hard
DEFS      = -DMISTER_BUILD=1 -DMISTER_NATIVE_VIDEO=1 \
            -DMISTER_WIDTH=$(FB_W) -DMISTER_HEIGHT=$(FB_H)
INCS      = -I$(VENDOR)/mister -I$(VENDOR)/mfgpu/host -I$(VENDOR)/mfgpu/refmodel -I$(SRCDIR)/misterglue
WARN      = -Wall -Wno-unused-function -Wno-unused-variable
COMMON    = -O2 -fPIC $(ARCHFLAGS) $(DEFS) $(INCS) $(WARN)
CXXFLAGS  = $(COMMON) -std=gnu++14 -fno-exceptions -fno-rtti
CFLAGS    = $(COMMON) -std=gnu11

VENDOR_CXX = $(VENDOR)/mister/raster_backend_mfgpu.cpp \
             $(VENDOR)/mister/raster_backend_sw.cpp \
             $(VENDOR)/mister/blitter_raster.cpp
VENDOR_C   = $(VENDOR)/mfgpu/host/blt_emitter.c \
             $(VENDOR)/mfgpu/host/blt_alloc.c \
             $(VENDOR)/mfgpu/refmodel/blitter_ref.c \
             $(VENDOR)/mfgpu/refmodel/blt_tri.c \
             $(VENDOR)/mister/native_video_writer.c
GLUE_CXX   = $(SRCDIR)/misterglue/glue.cpp \
             $(SRCDIR)/misterglue/godot_shadow.cpp

OBJS = $(patsubst %.cpp,$(OUT)/%.o,$(VENDOR_CXX) $(GLUE_CXX)) \
       $(patsubst %.c,$(OUT)/%.o,$(VENDOR_C))

all: $(OUT)/libmisterglue.so

$(OUT)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libmisterglue.so: $(OBJS)
	$(CXX) -shared -o $@ $(OBJS) -lm

# Compile only the vendored fabric stack — used to prove the vendoring boundary.
vendor-only: $(patsubst %.cpp,$(OUT)/%.o,$(VENDOR_CXX)) $(patsubst %.c,$(OUT)/%.o,$(VENDOR_C))
	@echo "vendored fabric stack compiles"

clean:
	rm -rf $(OUT)

.PHONY: all clean vendor-only
