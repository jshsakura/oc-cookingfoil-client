# SDL startup and devkitPro build structure adapted from oc-save-keeper.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro installation)
endif
include $(DEVKITPRO)/libnx/switch_rules
TARGET := cookingfoil
BUILD := build
SOURCES := source source/core source/net source/ui source/platform source/install source/i18n
ENGINE_ROOT := third_party/CyberFoil
ENGINE_DIRS := $(ENGINE_ROOT)/source/install $(ENGINE_ROOT)/source/nx $(ENGINE_ROOT)/source/nx/ipc $(ENGINE_ROOT)/source/data $(ENGINE_ROOT)/source/util
ENGINE_CPP := $(filter-out %/usb_nsp.cpp %/usb_xci.cpp %/sdmc_nsp.cpp %/sdmc_xci.cpp %/usbhdd.cpp,$(wildcard $(ENGINE_ROOT)/source/install/*.cpp $(ENGINE_ROOT)/source/nx/*.cpp $(ENGINE_ROOT)/source/data/*.cpp)) $(addprefix $(ENGINE_ROOT)/source/util/,crypto.cpp file_util.cpp title_util.cpp network_util.cpp)
ENGINE_C := $(wildcard $(ENGINE_ROOT)/source/nx/ipc/*.c) $(ENGINE_ROOT)/source/util/debug.c
QR_ROOT := third_party/QR-Code-generator/cpp
INCLUDES := engine-shim include $(ENGINE_ROOT)/include $(ENGINE_ROOT)/include/util $(QR_ROOT)
APP_TITLE := CookingFoil
APP_AUTHOR := OpenCourse
# The one place the version lives: the NACP, the HTTP Version header and the
# user agent all come from it, and a release tag must match it (vX.Y.Z).
APP_VERSION := 0.2.0
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ -DCOOKINGFOIL_VERSION=\"$(APP_VERSION)\" -I$(PORTLIBS)/include/SDL2 -I$(PORTLIBS)/include/freetype2
CXXFLAGS := $(CFLAGS) -std=c++17 -fno-rtti
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections,-Map,$(notdir $*.map)
LIBS := -lzstd -lSDL2_image -lSDL2_ttf `sdl2-config --libs` -lfreetype -lharfbuzz -lwebp -lpng -ljpeg `curl-config --libs` -lmbedtls -lmbedx509 -lmbedcrypto -ljson-c -lnx -lminizip -lz -lbz2
LIBDIRS := $(PORTLIBS) $(LIBNX)
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES) $(ENGINE_DIRS) $(QR_ROOT),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export OFILES := $(foreach dir,$(SOURCES),$(notdir $(patsubst %.cpp,%.o,$(wildcard $(dir)/*.cpp)))) $(notdir $(ENGINE_CPP:.cpp=.o) $(ENGINE_C:.c=.o)) qrcodegen.o
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export LD := $(CXX)
export NROFLAGS := --nacp=$(OUTPUT).nacp --icon=$(TOPDIR)/assets/icon.jpg --romfsdir=$(TOPDIR)/romfs
# The loader HOME Menu shortcuts start (forwarder/hbl, sphaira's hbl): an
# NSO and its NPDM, put in the RomFS for platform/Shortcut to embed.
HBL_SRC := forwarder/hbl
HBL_OUT := romfs/forwarder
HBL_FILES := $(HBL_OUT)/main $(HBL_OUT)/main.npdm
.PHONY: all $(BUILD)
all: $(BUILD)
$(BUILD): $(HBL_FILES)
$(BUILD)/hbl/hbl.elf: $(HBL_SRC)/main.c $(HBL_SRC)/trampoline.s
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -g -Wall $(ARCH) -ffunction-sections -D__SWITCH__ -DVERSION=\"3.0.0\" \
	  -I$(LIBNX)/include $^ -specs=$(LIBNX)/switch.specs -Wl,--gc-sections,-wrap,exit -L$(LIBNX)/lib -lnx -o $@
$(HBL_OUT)/main: $(BUILD)/hbl/hbl.elf
	@mkdir -p $(HBL_OUT)
	$(DEVKITPRO)/tools/bin/elf2nso $< $@
$(HBL_OUT)/main.npdm: $(HBL_SRC)/hbl.json
	@mkdir -p $(HBL_OUT)
	$(DEVKITPRO)/tools/bin/npdmtool $< $@
$(BUILD):
	mkdir -p $(BUILD)
	$(MAKE) --no-print-directory -C $(BUILD) -f $(TOPDIR)/makefiles/switch.mk
else
.PHONY: all
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(TOPDIR)/assets/icon.jpg $(wildcard $(TOPDIR)/romfs/fonts/*) $(TOPDIR)/romfs/forwarder/main $(TOPDIR)/romfs/forwarder/main.npdm
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
