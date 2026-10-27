# SDL startup and devkitPro build structure adapted from oc-save-keeper.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro installation)
endif
include $(DEVKITPRO)/libnx/switch_rules
TARGET := cookingfoil
BUILD := build
SOURCES := source source/core source/net source/ui source/platform source/mtp source/install source/i18n source/zstd
ENGINE_ROOT := third_party/CyberFoil
ENGINE_DIRS := $(ENGINE_ROOT)/source/install $(ENGINE_ROOT)/source/nx $(ENGINE_ROOT)/source/nx/ipc $(ENGINE_ROOT)/source/data $(ENGINE_ROOT)/source/util
ENGINE_CPP := $(filter-out %/usb_nsp.cpp %/usb_xci.cpp %/sdmc_nsp.cpp %/sdmc_xci.cpp %/usbhdd.cpp %/byte_stream.cpp,$(wildcard $(ENGINE_ROOT)/source/install/*.cpp $(ENGINE_ROOT)/source/nx/*.cpp $(ENGINE_ROOT)/source/data/*.cpp)) $(addprefix $(ENGINE_ROOT)/source/util/,crypto.cpp file_util.cpp title_util.cpp)
ENGINE_C := $(wildcard $(ENGINE_ROOT)/source/nx/ipc/*.c) $(ENGINE_ROOT)/source/util/debug.c
# libhaze (Atmosphère's haze as a library, patched by 0012 so a failure ends
# its thread instead of the app): the SD card over MTP from 연결 to PC.
HAZE_ROOT := $(ENGINE_ROOT)/external/libhaze
HAZE_CPP := $(wildcard $(HAZE_ROOT)/source/*.cpp)
QR_ROOT := third_party/QR-Code-generator/cpp
SQLITE_ROOT := third_party/sqlite
# zstd 1.5.7, decompression only (see the Makefile).
ZSTD_DIRS := third_party/zstd/lib/common third_party/zstd/lib/decompress
ZSTD_C := entropy_common.c error_private.c fse_decompress.c xxhash.c zstd_common.c huf_decompress.c zstd_ddict.c zstd_decompress.c zstd_decompress_block.c ZstdSnapshot.c
INCLUDES := engine-shim include $(ENGINE_ROOT)/include $(ENGINE_ROOT)/include/util $(HAZE_ROOT)/include $(QR_ROOT) $(SQLITE_ROOT) third_party/zstd/lib source/zstd
APP_TITLE := CookingFoil
APP_AUTHOR := OpenCourse
# The one place the version lives: the NACP, the HTTP Version header and the
# user agent all come from it, and a release tag must match it (vX.Y.Z).
# Raise it for every build that ships, so each one reads newer than the last
# (설정 > 앱 업데이트).
APP_VERSION := 0.5.15
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ -DCOOKINGFOIL_VERSION=\"$(APP_VERSION)\" -I$(PORTLIBS)/include/SDL2 -I$(PORTLIBS)/include/freetype2
CXXFLAGS := $(CFLAGS) -std=c++17 -fno-rtti
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections,-Map,$(notdir $*.map)
# curl and mbedTLS from deps/tls (scripts/build-tls.sh), ahead of the
# portlibs' older ones: TLS decrypted in the app on the CPU's AES
# instructions, not handed to the system's ssl service.
TLS := $(CURDIR)/deps/tls
LIBS := -lSDL2_image -lSDL2_ttf `sdl2-config --libs` -lfreetype -lharfbuzz -lwebp -lpng -ljpeg -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljson-c -lnx -lminizip -lz -lbz2
LIBDIRS := $(TLS) $(PORTLIBS) $(LIBNX)
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES) $(ENGINE_DIRS) $(HAZE_ROOT)/source $(QR_ROOT) $(SQLITE_ROOT) $(ZSTD_DIRS),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export OFILES := $(foreach dir,$(SOURCES),$(notdir $(patsubst %.cpp,%.o,$(wildcard $(dir)/*.cpp)))) $(notdir $(ENGINE_CPP:.cpp=.o) $(ENGINE_C:.c=.o) $(HAZE_CPP:.cpp=.o)) qrcodegen.o sqlite3.o $(ZSTD_C:.c=.o)
# Named here, where the wildcard sees the sources: the build folder's make
# takes the list from the environment.
export HAZE_O := $(notdir $(HAZE_CPP:.cpp=.o))
# Objects go in one folder by file name alone: two sources of the same name
# (source/i18n/es.cpp and the engine's nx/ipc/es.c) would leave one of them
# out, and the link fails far from the cause.
SAME_NAME := $(shell printf '%s\n' $(OFILES) | sort | uniq -d)
ifneq ($(SAME_NAME),)
$(error Two sources build the same object, rename one: $(SAME_NAME))
endif
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
$(BUILD): $(HBL_FILES) $(TLS)/lib/libcurl.a
$(TLS)/lib/libcurl.a: $(TOPDIR)/scripts/build-tls.sh
	$(TOPDIR)/scripts/build-tls.sh
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
# SQLite: its own warnings off; one thread uses it; no extensions, no WAL
# (it needs shared memory the console does not have).
sqlite3.o: CFLAGS += -w -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
$(ZSTD_C:.c=.o): CFLAGS += -w -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DDEBUGLEVEL=0 -DZSTD_TRACE=0 -I$(TOPDIR)/third_party/zstd/lib/common
# libhaze is written to C++20 (its own CMake asks for it).
$(HAZE_O): CXXFLAGS += -std=gnu++20 -w
-include $(OFILES:.o=.d)
endif
