# =============================================================================
# Makefile for AT32UC3B1 Custom HID Firmware
#
# Targets:
#   make all      - Build firmware.elf and firmware.hex
#   make clean    - Remove build artifacts
#   make flash    - Erase chip and flash firmware via dfu-programmer
#   make launch   - Launch the firmware after flashing
#   make erase    - Erase chip only (no flash)
#   make size     - Show section sizes
#
# Prerequisites (must be on PATH):
#   avr32-gcc         AVR32 GNU Compiler
#   avr32-objcopy     AVR32 objcopy (for ELF -> HEX conversion)
#   avr32-size        Section size reporter
#   dfu-programmer    DFU flashing tool
# =============================================================================

# ---- Toolchain ---------------------------------------------------------------
TC_BIN  = C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin
TC_INC  = C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/avr32/include
CC      = "$(TC_BIN)/avr32-gcc.exe"
AS      = "$(TC_BIN)/avr32-gcc.exe"
LD      = "$(TC_BIN)/avr32-gcc.exe"
OBJCOPY = "$(TC_BIN)/avr32-objcopy.exe"
SIZE    = "$(TC_BIN)/avr32-size.exe"
DFU     = "$(CURDIR)/tools/dfu-programmer.exe"

# ---- Target device (for dfu-programmer) --------------------------------------
DFU_TARGET = at32uc3b1256

# ---- Paths -------------------------------------------------------------------
SRC_DIR   = src
BUILD_DIR = build

# ---- Source files ------------------------------------------------------------
C_SRCS   = $(SRC_DIR)/main.c \
           $(SRC_DIR)/usb_hid.c \
           $(SRC_DIR)/usb_descriptors.c \
           $(SRC_DIR)/mmc.c \
           $(SRC_DIR)/pff.c \
           $(SRC_DIR)/usb_msc.c \
           $(SRC_DIR)/diagnose.c

ASM_SRCS = $(SRC_DIR)/startup.S

OBJS     = $(BUILD_DIR)/main.o        \
           $(BUILD_DIR)/usb_hid.o     \
           $(BUILD_DIR)/usb_descriptors.o \
           $(BUILD_DIR)/mmc.o         \
           $(BUILD_DIR)/pff.o         \
           $(BUILD_DIR)/usb_msc.o     \
           $(BUILD_DIR)/startup.o

# ---- Linker script -----------------------------------------------------------
LDSCRIPT = $(SRC_DIR)/at32uc3b1.ld

# ---- Output files ------------------------------------------------------------
TARGET_ELF = $(BUILD_DIR)/firmware.elf
TARGET_HEX = $(BUILD_DIR)/firmware.hex

# ---- Compiler flags ----------------------------------------------------------
MCU      = uc3b1256

CFLAGS   = -mpart=$(MCU)            \
            -isystem "$(TC_INC)"     \
            -O2                      \
            -Wall                    \
            -Wextra                  \
            -std=gnu99                 \
            -ffunction-sections      \
            -fdata-sections          \
            -ffreestanding           \
            -fno-builtin             \
            -I$(SRC_DIR)

ASFLAGS  = -mpart=$(MCU)            \
            -x assembler-with-cpp

LDFLAGS  = -mpart=$(MCU)            \
            -T$(LDSCRIPT)            \
            -Wl,--gc-sections        \
            -Wl,-Map=$(BUILD_DIR)/firmware.map \
            -nostartfiles            \
            -nostdlib                \
            -lc                      \
            -lgcc

# ---- Rules -------------------------------------------------------------------
.PHONY: all clean flash erase launch size

all: $(BUILD_DIR) $(TARGET_HEX)
	@echo ""
	@echo "========================================="
	@echo "  Build complete: $(TARGET_HEX)"
	@echo "========================================="
	@$(SIZE) $(TARGET_ELF)

$(BUILD_DIR):
	@cmd /c if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

# Compile C sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "[CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@





# Assemble startup file
$(BUILD_DIR)/startup.o: $(SRC_DIR)/startup.S
	@echo "[AS]  $<"
	$(CC) $(ASFLAGS) -c $< -o $@

# Link ELF
$(TARGET_ELF): $(OBJS)
	@echo "[LD]  $@"
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Convert ELF to Intel HEX
$(TARGET_HEX): $(TARGET_ELF)
	@echo "[HEX] $@"
	$(OBJCOPY) -O ihex $< $@

# ---- Flash targets -----------------------------------------------------------

erase:
	@echo ">>> Erasing $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) erase --force
	@echo ">>> Erase complete."

flash: all erase
	@echo ">>> Flashing $(TARGET_HEX) to $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) flash $(TARGET_HEX)
	@echo ">>> Flash complete."

launch:
	@echo ">>> Launching firmware on $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) launch
	@echo ">>> Device launched."

size: $(TARGET_ELF)
	$(SIZE) -A $<

clean:
	@echo ">>> Cleaning build directory..."
	rm -rf $(BUILD_DIR)
	@echo ">>> Done."
