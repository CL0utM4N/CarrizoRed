#
#  XHCIProbe build (macOS / CI). Needs MacKernelSDK in the repository root (scripts/bootstrap.sh fetches it).
#
#    make -C tools/XHCIProbe          -> tools/XHCIProbe/build/XHCIProbe.kext (+ zip)
#

PRODUCT_NAME   := XHCIProbe
MODULE_NAME    := com.carrizored.XHCIProbe
MODULE_VERSION := 0.2.0
MIN_MACOS      := 11.0

ROOT     := ../..
KSDK     := $(ROOT)/MacKernelSDK
BUILD    := build
KEXT     := $(BUILD)/$(PRODUCT_NAME).kext
BINARY   := $(KEXT)/Contents/MacOS/$(PRODUCT_NAME)

SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
TARGET  := -target x86_64-apple-macos$(MIN_MACOS) -isysroot $(SDKROOT)
DEFINES := -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-DMODULE_NAME=$(MODULE_NAME) -DMODULE_VERSION=$(MODULE_VERSION)
FLAGS   := $(TARGET) -Os $(DEFINES) -mkernel -nostdinc -nostdlib -fno-builtin -fno-common -static \
	-fno-asynchronous-unwind-tables -fno-stack-protector -I$(KSDK)/Headers -Wall -Wno-unknown-warning-option

.PHONY: all clean
all: $(BUILD)/$(PRODUCT_NAME)-$(MODULE_VERSION).zip

$(BUILD)/XHCIProbe.o: XHCIProbe.cpp | $(BUILD)
	xcrun clang++ $(FLAGS) -std=c++17 -fapple-kext -fno-rtti -fno-exceptions -c $< -o $@

$(BUILD)/kmod_info.o: kmod_info.c | $(BUILD)
	xcrun clang $(FLAGS) -std=c11 -c $< -o $@

$(BINARY): $(BUILD)/XHCIProbe.o $(BUILD)/kmod_info.o
	@mkdir -p $(dir $@)
	xcrun clang++ $(TARGET) -Xlinker -kext -nostdlib -static -L$(KSDK)/Library/x86_64 -lkmodc++ -lkmod -lcc_kext $^ -o $@

$(KEXT)/Contents/Info.plist: Info.plist.in Makefile
	@mkdir -p $(dir $@)
	sed -e 's/@MODULE_NAME@/$(MODULE_NAME)/g' -e 's/@MODULE_VERSION@/$(MODULE_VERSION)/g' $< > $@
	plutil -lint $@

$(BUILD)/$(PRODUCT_NAME)-$(MODULE_VERSION).zip: $(BINARY) $(KEXT)/Contents/Info.plist
	cd $(BUILD) && rm -f $(notdir $@) && zip -qr $(notdir $@) $(PRODUCT_NAME).kext

$(BUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILD)
