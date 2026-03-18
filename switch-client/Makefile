DEVKITPRO ?= /opt/devkitpro
export DEVKITPRO
TARGET := gbasync
BUILD := build
SOURCES := source/main.cpp

TOOLS := $(DEVKITPRO)/tools/bin
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CXX := $(PREFIX)g++
CC := $(PREFIX)gcc

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CPPFLAGS := -I$(DEVKITPRO)/libnx/include
CXXFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) -std=gnu++17 -fno-rtti -fno-exceptions
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) \
	-L$(DEVKITPRO)/libnx/lib \
	-Wl,-Map,$(TARGET).map
LIBS := -lnx

APP_TITLE := SaveSync
APP_AUTHOR := alexkirst
APP_VERSION := 0.1.1

OBJ := $(BUILD)/main.o

.PHONY: all clean

all: $(TARGET).nro

$(BUILD):
	@mkdir -p $(BUILD)

$(OBJ): $(SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJ)
	$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

$(TARGET).nacp:
	$(TOOLS)/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	$(TOOLS)/elf2nro $(TARGET).elf $@ --nacp=$(TARGET).nacp --icon=$(DEVKITPRO)/libnx/default_icon.jpg

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map
