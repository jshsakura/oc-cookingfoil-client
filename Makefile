.DEFAULT_GOAL := all
HOST_CXX ?= g++
HOST_PACKAGES := sdl2 SDL2_ttf SDL2_image json-c libcurl
HOST_FLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Iinclude $(shell pkg-config --cflags $(HOST_PACKAGES))
HOST_LIBS := $(shell pkg-config --libs $(HOST_PACKAGES)) -pthread
QR_ROOT := third_party/QR-Code-generator/cpp
HOST_FLAGS += -I$(QR_ROOT)
# makefiles/switch.mk holds major.minor; the last number is the commit
# count, so every build reads newer than the one before and 설정 > 앱
# 업데이트 can tell them apart (the server reads it from the NACP).
APP_BASE := $(shell sed -n 's/^APP_VERSION := //p' makefiles/switch.mk)
APP_BUILD := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
APP_VERSION := $(basename $(APP_BASE)).$(APP_BUILD)
HOST_FLAGS += -DCOOKINGFOIL_VERSION=\"$(APP_VERSION)\"
SHARED := $(wildcard source/core/*.cpp source/net/*.cpp source/ui/*.cpp source/install/*.cpp source/platform/*.cpp source/i18n/*.cpp) $(QR_ROOT)/qrcodegen.cpp
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
HEADERS := $(shell rg --files include 2>/dev/null) $(QR_ROOT)/qrcodegen.hpp
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
	docker run --rm -u "$$(id -u):$$(id -g)" -v "$(CURDIR)":/work -w /work devkitpro/devkita64 bash -lc 'make -f makefiles/switch.mk -j4 APP_VERSION=$(APP_VERSION)'
endif
host: build-host/cookingfoil-host
$(QR_ROOT)/qrcodegen.cpp:
	git submodule update --init third_party/QR-Code-generator
$(QR_ROOT)/qrcodegen.hpp: $(QR_ROOT)/qrcodegen.cpp
	@test -f $@
build-host/cookingfoil-host: $(wildcard host/*.cpp host/*.hpp) $(SHARED) $(HEADERS)
	mkdir -p build-host
	$(HOST_CXX) $(HOST_FLAGS) -I. $(wildcard host/*.cpp) $(SHARED) -o $@ $(HOST_LIBS)
build-host/unit-tests: $(wildcard tests/*.cpp) $(SHARED) $(HEADERS)
	mkdir -p build-host
	$(HOST_CXX) $(HOST_FLAGS) $(wildcard tests/*.cpp) $(SHARED) -o $@ $(HOST_LIBS)
test: build-host/unit-tests
	SDL_VIDEODRIVER=dummy ./build-host/unit-tests
clean:
	rm -rf build build-host cookingfoil.nro cookingfoil.nacp cookingfoil.elf

engine-patch:
	@test -f third_party/CyberFoil/include/install/install.hpp || git submodule update --init third_party/CyberFoil
	bash third_party/apply-patches.sh
