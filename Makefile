.DEFAULT_GOAL := all
# Every core works by default; `make -j1` still runs one at a time.
JOBS := $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(JOBS)
HOST_CXX ?= g++
HOST_CC ?= gcc
HOST_PACKAGES := sdl2 SDL2_ttf SDL2_image json-c libcurl zlib libjpeg libcrypto
HOST_FLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Iinclude $(shell pkg-config --cflags $(HOST_PACKAGES))
HOST_LIBS := $(shell pkg-config --libs $(HOST_PACKAGES)) -pthread
QR_ROOT := third_party/QR-Code-generator/cpp
HOST_FLAGS += -I$(QR_ROOT) -Ithird_party/sqlite -Ithird_party/zstd/lib -Isource/zstd
# SQLite (third_party/sqlite, the amalgamation): built once as C with its own
# warnings off; no extensions, so nothing loads code at run time.
SQLITE_FLAGS := -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
# zstd's decoder (third_party/zstd, 1.5.7, decompression only), fixed so a
# saved decoder state (source/zstd) reads back the same in every build.
ZSTD_C := $(addprefix third_party/zstd/lib/common/,entropy_common.c error_private.c fse_decompress.c xxhash.c zstd_common.c) \
          $(addprefix third_party/zstd/lib/decompress/,huf_decompress.c zstd_ddict.c zstd_decompress.c zstd_decompress_block.c) \
          source/zstd/ZstdSnapshot.c
ZSTD_FLAGS := -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DDEBUGLEVEL=0 -DZSTD_TRACE=0 -Ithird_party/zstd/lib -Ithird_party/zstd/lib/common
# The version as makefiles/switch.mk writes it (raised by hand for a release):
# not the commit count, which a fresh clone, a shallow CI checkout or the
# public repository's history would each read differently.
APP_VERSION := $(shell sed -n 's/^APP_VERSION := //p' makefiles/switch.mk)
HOST_FLAGS += -DCOOKINGFOIL_VERSION=\"$(APP_VERSION)\"
SHARED := $(wildcard source/core/*.cpp source/net/*.cpp source/ui/*.cpp source/install/*.cpp source/platform/*.cpp source/i18n/*.cpp source/zstd/*.cpp) $(QR_ROOT)/qrcodegen.cpp
# Save backups zip with minizip. Without it on the host (Debian/Ubuntu package
# libminizip-dev) the zip code and its archive tests are left out.
ifeq ($(shell pkg-config --exists minizip && echo yes),yes)
HOST_FLAGS += $(shell pkg-config --cflags minizip)
HOST_LIBS += $(shell pkg-config --libs minizip)
else
$(warning minizip not found: host build skips source/core/Zip.cpp (install libminizip-dev))
SHARED := $(filter-out source/core/Zip.cpp,$(SHARED))
HOST_FLAGS += -DFOIL_NO_MINIZIP
endif
.PHONY: all host test clean switch-build engine-patch version
version:
	@echo $(APP_VERSION)
all: switch-build
switch-build: engine-patch $(QR_ROOT)/qrcodegen.cpp
	@# A new version rebuilds what carries it: the NACP and the files that
	@# include core/Version.hpp (a changed -D alone does not trigger make).
	@if [ "$$(cat build/version.stamp 2>/dev/null)" != "$(APP_VERSION)" ]; then \
	  rm -f cookingfoil.nacp $(addprefix build/,$(notdir $(patsubst %.cpp,%.o,$(shell grep -rl 'core/Version.hpp' source)))); \
	  mkdir -p build && echo "$(APP_VERSION)" > build/version.stamp; fi
ifdef DEVKITPRO
	$(MAKE) -f makefiles/switch.mk APP_VERSION=$(APP_VERSION)
else
	docker run --rm -u "$$(id -u):$$(id -g)" -v "$(CURDIR)":/work -w /work devkitpro/devkita64 bash -lc 'make -f makefiles/switch.mk -j$(JOBS) APP_VERSION=$(APP_VERSION)'
endif
host: build-host/cookingfoil-host
$(QR_ROOT)/qrcodegen.cpp:
	git submodule update --init third_party/QR-Code-generator
$(QR_ROOT)/qrcodegen.hpp: $(QR_ROOT)/qrcodegen.cpp
	@test -f $@
# One object per source with its header dependencies, built in parallel:
# a change recompiles only what it touches, and the tests and the preview
# share the app's objects instead of compiling everything twice.
HOST_OBJ := build-host/obj
host_obj = $(addprefix $(HOST_OBJ)/,$(patsubst %.cpp,%.o,$(1)))
SHARED_OBJ := $(call host_obj,$(SHARED)) $(HOST_OBJ)/third_party/sqlite/sqlite3.o $(addprefix $(HOST_OBJ)/,$(ZSTD_C:.c=.o))
HOST_APP_OBJ := $(call host_obj,$(wildcard host/*.cpp))
TEST_OBJ := $(call host_obj,$(wildcard tests/*.cpp))
# The version is a -D flag, which make does not see change: the files that
# read it depend on a stamp rewritten only when the version moves.
VERSION_STAMP := build-host/version.stamp
$(shell mkdir -p build-host; [ "$$(cat $(VERSION_STAMP) 2>/dev/null)" = "$(APP_VERSION)" ] || echo "$(APP_VERSION)" > $(VERSION_STAMP))
$(call host_obj,$(shell grep -rl 'core/Version.hpp' source host tests 2>/dev/null)): $(VERSION_STAMP)
$(HOST_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_FLAGS) -I. -MMD -MP -c $< -o $@
$(HOST_OBJ)/third_party/sqlite/sqlite3.o: third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(SQLITE_FLAGS) -DSQLITE_THREADSAFE=0 -c $< -o $@
$(addprefix $(HOST_OBJ)/,$(ZSTD_C:.c=.o)): $(HOST_OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(ZSTD_FLAGS) -c $< -o $@
-include $(SHARED_OBJ:.o=.d) $(HOST_APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
build-host/cookingfoil-host: $(HOST_APP_OBJ) $(SHARED_OBJ)
	$(HOST_CXX) $^ -o $@ $(HOST_LIBS)
build-host/unit-tests: $(TEST_OBJ) $(SHARED_OBJ)
	$(HOST_CXX) $^ -o $@ $(HOST_LIBS)
test: build-host/unit-tests
	SDL_VIDEODRIVER=dummy ./build-host/unit-tests
clean:
	rm -rf build build-host cookingfoil.nro cookingfoil.nacp cookingfoil.elf

engine-patch:
	@test -f third_party/CyberFoil/include/install/install.hpp || git submodule update --init third_party/CyberFoil
	bash third_party/apply-patches.sh
