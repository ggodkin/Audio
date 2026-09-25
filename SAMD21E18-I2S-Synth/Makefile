TARGET := samd21e18_i2s_synth
BUILD := build
SRC := src
INC := include

CROSS ?= arm-none-eabi
CC := $(CROSS)-gcc
OBJCOPY := $(CROSS)-objcopy
SIZE := $(CROSS)-size

CMSIS_CORE ?= $(HOME)/src/CMSIS_5/CMSIS/Core/Include
SAMD21_CMSIS ?= $(HOME)/src/CMSIS-Atmel/CMSIS/CMSIS/Device/ATMEL/samd21

CFLAGS := -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft
CFLAGS += -Os -ffunction-sections -fdata-sections
CFLAGS += -ffreestanding -fno-builtin
CFLAGS += -D__SAMD21E18A__ -D__ATSAMD21E18A__
CFLAGS += -I$(INC) -I$(CMSIS_CORE) -I$(SAMD21_CMSIS)/include

LDFLAGS := -mcpu=cortex-m0plus -mthumb
LDFLAGS += -Tlinker.ld -nostdlib -Wl,--gc-sections
LDFLAGS += -Wl,-Map,$(BUILD)/$(TARGET).map

SYSTEM_SRC := $(SAMD21_CMSIS)/source/system_samd21.c
OBJS := $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/system_samd21.o

OPENOCD ?= openocd
OPENOCD_CFG ?= interface/stlink.cfg
OPENOCD_TARGET ?= target/at91samdXX.cfg

.PHONY: all clean flash size

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin size

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/startup.o: $(SRC)/startup.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/main.o: $(SRC)/main.c $(INC)/audio_config.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/system_samd21.o: $(SYSTEM_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

flash: $(BUILD)/$(TARGET).bin
	$(OPENOCD) -f $(OPENOCD_CFG) -f $(OPENOCD_TARGET) \
		-c "program $(BUILD)/$(TARGET).bin verify reset exit 0x00000000"

clean:
	rm -rf $(BUILD)
