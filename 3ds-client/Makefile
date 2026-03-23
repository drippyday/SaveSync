DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO
export DEVKITARM
TARGET := gbasync
BUILD := build
SOURCES := source/main.c

TOOLS := $(DEVKITPRO)/tools/bin
PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
CC := $(PREFIX)gcc

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CPPFLAGS := -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include
CFLAGS := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
LDFLAGS := -specs=3dsx.specs $(ARCH) \
	-L$(DEVKITPRO)/libctru/lib -L$(DEVKITPRO)/portlibs/3ds/lib \
	-Wl,-Map,$(TARGET).map
LIBS := -lctru -lm

APP_TITLE := GBAsync
APP_DESCRIPTION := GBA save sync client
APP_AUTHOR := drippyday
CIA_VERSION_HEX ?= 0x2710
ICON_SRC := ../icons/3ds_icon_48x48-2.png
BANNER_SRC := ../icons/3ds_banner_256x128-2.png
ICON_PNG := $(ICON_SRC)
BANNER_PNG := $(BUILD)/app_banner_256x128.png
BANNER_BNR := $(BUILD)/app_banner.bnr
SILENCE_WAV := $(BUILD)/banner_silence.wav
ICON_ICN := $(BUILD)/app_icon.icn
RSF := $(BUILD)/app.rsf

OBJ := $(BUILD)/main.o

.PHONY: all clean

all: $(TARGET).3dsx $(TARGET).cia

$(BUILD):
	@mkdir -p $(BUILD)

$(OBJ): $(SOURCES) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BANNER_PNG): | $(BUILD)
	@if command -v sips >/dev/null 2>&1; then \
		if [ -f "$(BANNER_SRC)" ]; then \
			sips -z 128 256 -s format png "$(BANNER_SRC)" --out "$@" >/dev/null; \
		else \
			echo "WARN: custom icon not found; using default libctru icon"; \
			sips -z 128 256 -s format png "$(DEVKITPRO)/libctru/default_icon.png" --out "$@" >/dev/null; \
		fi; \
	else \
		echo "ERROR: sips is required to generate banner PNG for 3DS"; \
		exit 1; \
	fi

$(TARGET).elf: $(OBJ)
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

$(TARGET).smdh: $(ICON_PNG)
	$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" "$(ICON_PNG)" $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	$(TOOLS)/3dsxtool $(TARGET).elf $@

$(ICON_ICN): $(ICON_PNG) | $(BUILD)
	bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i "$(ICON_PNG)" -o "$@"

$(SILENCE_WAV): | $(BUILD)
	@python3 -c 'import struct,wave; p="$(SILENCE_WAV)"; w=wave.open(p,"wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050); w.writeframes(b"".join(struct.pack("<h",0) for _ in range(22050))); w.close()'

$(BANNER_BNR): $(BANNER_PNG) $(SILENCE_WAV) | $(BUILD)
	bannertool makebanner -i "$(BANNER_PNG)" -a "$(SILENCE_WAV)" -o "$@"

$(RSF): | $(BUILD)
	@printf '%s\n' \
		'BasicInfo:' \
		'  Title                   : "$(APP_TITLE)"' \
		'  CompanyCode             : "00"' \
		'  ProductCode             : "CTR-H-SYNC"' \
		'  ContentType             : Application' \
		'  Logo                    : Nintendo' \
		'' \
		'TitleInfo:' \
		'  UniqueId                : 0x5A5E0' \
		'  Category                : Application' \
		'  Version                 : $(CIA_VERSION_HEX)' \
		'' \
		'Option:' \
		'  EnableCrypt             : False' \
		'  FreeProductCode         : True' \
		'  MediaFootPadding        : False' \
		'  UseOnSD                 : True' \
		'  EnableCompress          : True' > "$@"

$(TARGET).cia: $(TARGET).elf $(ICON_ICN) $(BANNER_BNR) $(RSF)
	makerom -f cia -target t -o "$@" -elf "$(TARGET).elf" -rsf "$(RSF)" -icon "$(ICON_ICN)" -banner "$(BANNER_BNR)" -desc app:2 -exefslogo -DAPP_ENCRYPTED=false

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia $(TARGET).smdh $(TARGET).map
