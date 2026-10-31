#
#  CarrizoRed build
#
#  Builds the kext with plain clang on macOS (no Xcode project needed).
#  Run ./scripts/bootstrap.sh once first to fetch Lilu.kext and MacKernelSDK.
#
#    make            Release build -> build/Release/CarrizoRed.kext
#    make DEBUG=1    Debug build   -> build/Debug/CarrizoRed.kext (enables DBGLOG)
#    make zip        Zips the kext (and dSYM-less debug info) for distribution
#    make clean
#

PRODUCT_NAME   := CarrizoRed
MODULE_NAME    := com.carrizored.$(PRODUCT_NAME)
MODULE_VERSION := 0.1.0
MIN_MACOS      := 11.0
# The commit, in the disk log header (which build wrote a log).
GIT_COMMIT     := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

SRC_DIR  := $(PRODUCT_NAME)
KSDK     := MacKernelSDK
LILU_SDK := Lilu.kext/Contents/Resources

ifeq ($(DEBUG),1)
CONFIG  := Debug
OPTFLAGS := -O0 -g -DDEBUG=1
else
CONFIG  := Release
OPTFLAGS := -Os
endif

BUILD_DIR := build/$(CONFIG)
OBJ_DIR   := $(BUILD_DIR)/obj
KEXT      := $(BUILD_DIR)/$(PRODUCT_NAME).kext
BINARY    := $(KEXT)/Contents/MacOS/$(PRODUCT_NAME)

SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
CC      := xcrun clang
CXX     := xcrun clang++

TARGET := -target x86_64-apple-macos$(MIN_MACOS) -isysroot $(SDKROOT)

DEFINES := \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-DPRODUCT_NAME=$(PRODUCT_NAME) \
	-DMODULE_VERSION=$(MODULE_VERSION) \
	-DMODULE_NAME=$(MODULE_NAME) \
	-DCR_COMMIT=$(GIT_COMMIT)

# Mirrors the flags acidanthera's Lilu plugins use in their Xcode projects.
COMMON_FLAGS := $(TARGET) $(OPTFLAGS) $(DEFINES) \
	-mkernel -nostdinc -nostdlib -fno-builtin -fno-common -static \
	-mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse \
	-fno-asynchronous-unwind-tables -fno-stack-protector \
	-I$(KSDK)/Headers -I$(LILU_SDK) -I$(SRC_DIR) \
	-Wall -Wno-unknown-warning-option -Wno-ossharedptr-misuse -Wno-vla -Wno-c99-designator

CFLAGS   := $(COMMON_FLAGS) -std=c11
CXXFLAGS := $(COMMON_FLAGS) -std=c++17 -fapple-kext -fno-rtti -fno-exceptions

LDFLAGS := $(TARGET) -Xlinker -kext -nostdlib -static \
	-L$(KSDK)/Library/x86_64 -lkmodc++ -lkmod -lcc_kext

CXX_SRCS := $(wildcard $(SRC_DIR)/*.cpp) $(LILU_SDK)/Library/plugin_start.cpp
C_SRCS   := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst %,$(OBJ_DIR)/%.o,$(notdir $(CXX_SRCS) $(C_SRCS)))

vpath %.cpp $(SRC_DIR) $(LILU_SDK)/Library
vpath %.c   $(SRC_DIR)

.PHONY: all clean zip check-sdk

all: check-sdk $(KEXT)

check-sdk:
	@test -f $(LILU_SDK)/Headers/kern_api.hpp || { echo "Lilu SDK missing - run ./scripts/bootstrap.sh"; exit 1; }
	@test -f $(KSDK)/Library/x86_64/libkmod.a || { echo "MacKernelSDK missing - run ./scripts/bootstrap.sh"; exit 1; }

$(OBJ_DIR)/%.cpp.o: %.cpp $(wildcard $(SRC_DIR)/*.hpp) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.c.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -DKMOD_START=$(PRODUCT_NAME)_kern_start -DKMOD_STOP=$(PRODUCT_NAME)_kern_stop -c $< -o $@

$(BINARY): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

$(KEXT)/Contents/Info.plist: $(SRC_DIR)/Info.plist.in Makefile
	@mkdir -p $(dir $@)
	sed -e 's/@PRODUCT_NAME@/$(PRODUCT_NAME)/g' \
	    -e 's/@MODULE_NAME@/$(MODULE_NAME)/g' \
	    -e 's/@MODULE_VERSION@/$(MODULE_VERSION)/g' $< > $@
	plutil -lint $@

$(KEXT): $(BINARY) $(KEXT)/Contents/Info.plist
	@touch $@
	@echo "Built $@"

$(OBJ_DIR):
	@mkdir -p $@

zip: all
	cd $(BUILD_DIR) && rm -f $(PRODUCT_NAME)-$(MODULE_VERSION)-$(CONFIG).zip && \
		zip -qr $(PRODUCT_NAME)-$(MODULE_VERSION)-$(CONFIG).zip $(PRODUCT_NAME).kext

clean:
	rm -rf build
