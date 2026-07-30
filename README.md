# AT32UC3B1 USB Rubber Ducky Firmware

Keystroke-injection firmware for the **Atmel AT32UC3B1** (AVR32 UC3B1256)
microcontroller. On boot it mounts the SD card over SPI, reads `INJECT.BIN` into
RAM, and interprets it as a stream of 16-bit big-endian words — each word is
either a control opcode (DELAY, GOTO, IF, ATTACKMODE, WAIT_FOR_BUTTON_PRESS, …)
or a raw HID keystroke encoded as `(keycode << 8) | modifier`. The device
enumerates as a **HID keyboard only**; the SD card is never exposed to the host
(see the design note under *Reliability* and `FIXES.md`).

> **Bug fixes:** see [`FIXES.md`](FIXES.md) for the recent fixes (HID-only force,
> extended DELAY, keycode plausibility gate) and the two Python verification tools
> in `tools/` (`firmware_sim.py`, `inject_inspect.py`).

> **Note:** parts of the older sections below (the "HID Report Protocol"
> heartbeat/LED command table and the "Add SD Card Support" note) describe an
> earlier generic-HID demo and **do not apply** to this Rubber Ducky build — the
> SD card is central here, not optional. They are kept only for historical
> reference and will be pruned.

---

## Folder Structure

```
firmware/
├── src/
│   ├── main.c              # Application logic, HID command handler, heartbeat
│   ├── usb_hid.c           # USB USBB peripheral driver + HID stack
│   ├── usb_hid.h           # USB HID API declarations
│   ├── usb_device.h        # USB device layer header
│   ├── usb_descriptors.c   # USB Device / Config / HID / String descriptors
│   ├── usb_descriptors.h   # Descriptor size/extern declarations
│   ├── startup.S           # AVR32 vector table + startup code (reset handler)
│   └── at32uc3b1.ld        # Linker script (app starts at 0x80002000, after DFU BL)
├── build/                  # Output directory (created by make)
│   ├── firmware.elf
│   ├── firmware.hex        ← This is what you flash
│   └── firmware.map
├── tools/                  # Place dfu-programmer.exe here
├── scripts/
│   ├── flash_firmware.bat  # One-click: Erase + Flash + Launch
│   └── erase_only.bat      # Erase only (for bricked devices)
├── Makefile
└── README.md
```

---

## USB Device Identity

| Property       | Value                          |
|----------------|--------------------------------|
| Vendor ID      | `0x03EB` (Atmel)               |
| Product ID     | `0x2402`                       |
| Device Class   | HID (Generic)                  |
| IN Report      | 64 bytes, Report ID `0x02`     |
| OUT Report     | 64 bytes, Report ID `0x01`     |
| Polling Rate   | 1 ms                           |

> **Note:** For production use, obtain your own VID/PID from USB-IF or use the 
> Microchip USB VID sublicense program.

---

## HID Report Protocol

### OUT Report (Host → Device) — Report ID `0x01`

| Byte | Description                        |
|------|------------------------------------|
| 0    | Report ID = `0x01`                 |
| 1    | Command code (see below)           |
| 2–63 | Command payload (command-specific) |

| Command | Code   | Description                                        |
|---------|--------|----------------------------------------------------|
| NOP     | `0x00` | Ping — device replies with ACK                     |
| LED ON  | `0x01` | Turn on the status LED                             |
| LED OFF | `0x02` | Turn off the status LED                            |
| LED TOG | `0x03` | Toggle the status LED                              |
| ECHO    | `0x10` | Device echoes bytes 2–63 back in an IN report      |

### IN Report (Device → Host) — Report ID `0x02`

| Byte  | Description                              |
|-------|------------------------------------------|
| 0     | Report ID = `0x02`                       |
| 1     | Status: `0x01` = alive, `0xFF` = error   |
| 2–5   | 32-bit uptime in ms (little-endian)      |
| 6     | Button state: `0x01` pressed             |
| 7–63  | Reserved / echo payload                  |

The device automatically sends a heartbeat IN report every **100 ms**.

---

## Prerequisites

### 1. AVR32 GNU Toolchain

Download from one of:
- **Microchip (official, Windows installer):**  
  https://www.microchip.com/en-us/tools-resources/develop/microchip-studio/gcc-compilers
- **Embecosm (buildable from source):**  
  https://github.com/embecosm/avr32-toolchain

Install and ensure these are on your `PATH`:
```
avr32-gcc
avr32-objcopy
avr32-size
```

Verify with:
```cmd
avr32-gcc --version
```

### 2. dfu-programmer (Windows)

Download the latest release from:  
https://sourceforge.net/projects/dfu-programmer/files/

Place `dfu-programmer.exe` in the `tools\` folder, **or** add it to your system `PATH`.

### 3. WinUSB Driver for DFU Device (Zadig)

The AT32UC3B1 DFU bootloader uses a custom USB driver on Windows.  
You need to replace it with **WinUSB** using **Zadig**:

1. Download Zadig: https://zadig.akeo.ie/
2. Plug in your device while holding the BOOT/HWB button to enter DFU mode
3. Open Zadig → `Options` → `List All Devices`
4. Select `AT32UC3B DFU` (or similar) from the dropdown
5. Select `WinUSB` as the driver → Click **Install Driver**
6. Verify the device now shows as `WinUSB` in Device Manager

### 4. Make (Windows)

Install via:
- **Chocolatey:** `choco install make`
- **Git for Windows / MSYS2:** comes with `make`
- Or use **WSL** and run `make` from there

---

## Building the Firmware

```cmd
cd C:\Users\flori\Downloads\firmware
make all
```

Output files:
- `build/firmware.elf` — ELF debug binary
- `build/firmware.hex` — Intel HEX file for flashing
- `build/firmware.map` — Linker map

---

## Flashing

### Option A: One-Click Script (Recommended)

```
scripts\flash_firmware.bat   (Run as Administrator)
```

### Option B: Manual dfu-programmer Commands

Open Command Prompt **as Administrator** in the `firmware` folder:

```cmd
:: Step 1 — Erase (chip is bricked/blank, --force skips the "blank check" error)
dfu-programmer at32uc3b1 erase --force

:: Step 2 — Flash
dfu-programmer at32uc3b1 flash build\firmware.hex

:: Step 3 — Launch (resets the device to run user firmware)
dfu-programmer at32uc3b1 launch
```

After launch, the device will disconnect from DFU mode and re-enumerate as a
**Generic HID device**. The status LED will light up solid when enumeration completes.

### Option C: Erase Only (for bricked chip)

```
scripts\erase_only.bat   (Run as Administrator)
```

---

## Verifying / Testing the HID Device

After flashing, use **hidapitester** to verify communication:

```cmd
:: List HID devices (find VID 03EB PID 2402)
hidapitester --list

:: Read a heartbeat report (64 bytes) from the device
hidapitester --vidpid 03EB:2402 --read-input 64 --length 64

:: Send LED ON command
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x01 --length 64

:: Send LED OFF command
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x02 --length 64

:: Send ECHO command (echo "Hello")
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x10,0x48,0x65,0x6C,0x6C,0x6F --length 64
```

Download hidapitester: https://github.com/todbot/hidapitester/releases

---

## Memory Map

| Region          | Start        | End          | Size    | Purpose                    |
|-----------------|--------------|--------------|---------|----------------------------|
| Flash (DFU BL)  | `0x80000000` | `0x80001FFF` | 8 KB    | Factory DFU bootloader     |
| Flash (App)     | `0x80002000` | `0x8003FFFF` | 248 KB  | Your firmware (this code)  |
| SRAM            | `0x00000000` | `0x00007FFF` | 32 KB   | Stack + data + BSS         |
| USBB FIFO       | `0xFFFE0000` | `0xFFFEFFFF` | 64 KB   | USB peripheral FIFOs       |

> The linker script (`at32uc3b1.ld`) ensures your application is placed starting  
> at `0x80002000`, preserving the DFU bootloader permanently.

---

## Customization

### Change LED / Button Pins

In `src/main.c`, update:
```c
#define LED_PIN    AVR32_PIN_PB00   // Change to your board's LED pin
#define BUTTON_PIN AVR32_PIN_PB01   // Change to your board's button pin
```

### Add More HID Commands

In `src/main.c`, extend the `switch(cmd)` block in `process_hid_out_report()`.

### Change VID/PID

In `src/usb_descriptors.h`:
```c
#define USB_VENDOR_ID   0x03EB   // Your VID
#define USB_PRODUCT_ID  0x2402   // Your PID
```

### SD Card (already in use — historical note corrected)

> The claim that "the SD card slot is not currently used" applied to the old
> demo firmware. **This build uses the SD card as its core input**: `main.c`
> mounts it with Petit FatFs (`pf_mount`) and reads `INJECT.BIN` at boot
> (`src/pff.c`, `src/mmc.c`). The compiled payload lives on the card; the host
> never sees the card as a drive.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "No device present" | Ensure device is in DFU mode; install WinUSB via Zadig |
| "dfu-programmer not found" | Place in `tools\` or add to PATH |
| LED doesn't light after flash | Check pin definitions in `main.c` |
| HID device not appearing | Unplug/replug; check VID/PID with Device Manager |
| Build error: avr32-gcc not found | Install AVR32 GNU Toolchain and add to PATH |
| Build error: io.h not found | Install AVR32 headers package alongside toolchain |
