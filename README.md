# AT32UC3B1 USB Rubber Ducky Firmware

Keystroke-injection firmware for the Atmel AT32UC3B1 (AVR32 UC3B1256) microcontroller. On boot it mounts the SD card over SPI, reads `INJECT.BIN` into RAM, and interprets it as a stream of 16-bit big-endian words. Each word is either a control opcode (DELAY, GOTO, IF, ATTACKMODE, WAIT_FOR_BUTTON_PRESS, and others) or a raw HID keystroke encoded as `(keycode << 8) | modifier`. The device enumerates as a HID keyboard only; the SD card is never exposed to the host.

Reference material for the payload language, keymap format, and firmware behavior lives in `info/`. Bench-test payloads and storage/end-to-end tests live in `hwtest/`.

## Folder Structure

```
.
├── src/                        Firmware source
│   ├── main.c                  Application logic, HID command handler, heartbeat
│   ├── usb_hid.c/h             USB USBB peripheral driver and HID stack
│   ├── usb_device.h            USB device layer header
│   ├── usb_descriptors.c/h     USB Device / Config / HID / String descriptors
│   ├── startup.S               AVR32 vector table and startup code
│   ├── at32uc3b1.ld            Linker script (app starts at 0x80002000, after DFU BL)
│   ├── pff.c, mmc.c            Petit FatFs and SD card driver
│   └── ...
├── info/                       Reference documentation
│   ├── DUCKYSCRIPT_COVERAGE.md
│   ├── Instructions1.txt
│   ├── HowLanguageFilesWork(Instructions2).txt
│   └── HowTheFirmwareUnderstandsTheKeymaps(Instructions3).txt
├── hwtest/                     Bench-test payloads
│   ├── test_payload.txt        (was inject.txt)
│   ├── STORAGE_TEST.txt
│   ├── ULTIMATE_TEST.txt
│   ├── 1_jitter.txt, 2_literal_delay.txt, 3_reflection.txt
│   ├── all_commands.txt, kitchen_sink.txt, mega_test.txt, modifier_test.txt
│   └── real_test.txt
├── languages/                  Keymap JSON files (US, DE, FR, ...)
├── tools/                      Host-side tooling
│   ├── AVR Toolchain/          AVR32 GNU Toolchain (Windows build)
│   ├── dfu-programmer.exe
│   ├── dfu-programmer-win.zip
│   ├── dfu-programmer-x64.zip
│   ├── dfu-programmer-x64-extracted/
│   ├── inject_inspect.py       Payload inspector
│   ├── wrap_inject.py          Wraps compiled payloads into INJECT.BIN
│   ├── libusb-1.0.dll
│   └── licenses/
├── build/                      Output directory (created by make)
│   ├── firmware.elf
│   ├── firmware.hex            This is what you flash
│   └── firmware.map
├── CONFIG.TXT
├── flash_firmware.bat          One-click: erase, flash, launch
├── Makefile
└── README.md
```

## USB Device Identity

| Property     | Value                          |
|--------------|--------------------------------|
| Vendor ID    | `0x03EB` (Atmel)               |
| Product ID   | `0x2402`                       |
| Device Class | HID (Keyboard)                 |
| IN Report    | 64 bytes, Report ID `0x02`     |
| OUT Report   | 64 bytes, Report ID `0x01`     |
| Polling Rate | 1 ms                           |

For production use, obtain your own VID/PID from USB-IF or use the Microchip USB VID sublicense program.

## HID Report Protocol

### OUT Report (Host to Device), Report ID `0x01`

| Byte | Description                        |
|------|------------------------------------|
| 0    | Report ID = `0x01`                 |
| 1    | Command code (see below)           |
| 2-63 | Command payload (command-specific) |

| Command | Code   | Description                                        |
|---------|--------|----------------------------------------------------|
| NOP     | `0x00` | Ping; device replies with ACK                      |
| LED ON  | `0x01` | Turn on the status LED                             |
| LED OFF | `0x02` | Turn off the status LED                            |
| LED TOG | `0x03` | Toggle the status LED                              |
| ECHO    | `0x10` | Device echoes bytes 2-63 back in an IN report      |

### IN Report (Device to Host), Report ID `0x02`

| Byte  | Description                              |
|-------|------------------------------------------|
| 0     | Report ID = `0x02`                       |
| 1     | Status: `0x01` = alive, `0xFF` = error   |
| 2-5   | 32-bit uptime in ms (little-endian)      |
| 6     | Button state: `0x01` pressed             |
| 7-63  | Reserved or echo payload                 |

The device sends a heartbeat IN report every 100 ms.

## Prerequisites

### 1. AVR32 GNU Toolchain

A Windows build is included at `tools/AVR Toolchain/`. Add its `bin/` directory to your `PATH`, or install a fresh copy from:
- Microchip (official Windows installer): https://www.microchip.com/en-us/tools-resources/develop/microchip-studio/gcc-compilers
- Embecosm (buildable from source): https://github.com/embecosm/avr32-toolchain

Confirm that these are on `PATH`:
```
avr32-gcc
avr32-objcopy
avr32-size
```

Verify:
```cmd
avr32-gcc --version
```

### 2. dfu-programmer (Windows)

Already vendored at `tools/dfu-programmer.exe`. Fresh copy: https://sourceforge.net/projects/dfu-programmer/files/

### 3. WinUSB Driver for the DFU Device (Zadig)

The AT32UC3B1 DFU bootloader uses a custom USB driver on Windows. Replace it with WinUSB using Zadig:

1. Download Zadig: https://zadig.akeo.ie/
2. Plug in the device while holding the BOOT/HWB button to enter DFU mode
3. Open Zadig, `Options`, `List All Devices`
4. Select `AT32UC3B DFU` (or similar) from the dropdown
5. Select `WinUSB` as the driver, click `Install Driver`
6. Verify the device now shows as `WinUSB` in Device Manager

### 4. Make (Windows)

- Chocolatey: `choco install make`
- Git for Windows or MSYS2: comes with `make`
- Or WSL, and run `make` from there

## Building the Firmware

```cmd
make all
```

Output:
- `build/firmware.elf` ELF debug binary
- `build/firmware.hex` Intel HEX for flashing
- `build/firmware.map` Linker map

## Flashing

### Option A: one-click script

```
flash_firmware.bat   (run as Administrator)
```

### Option B: manual dfu-programmer commands

Open a Command Prompt as Administrator in the project root:

```cmd
:: Step 1: erase (chip is bricked or blank; --force skips the blank check error)
dfu-programmer at32uc3b1 erase --force

:: Step 2: flash
dfu-programmer at32uc3b1 flash build\firmware.hex

:: Step 3: launch (resets the device to run user firmware)
dfu-programmer at32uc3b1 launch
```

After launch, the device disconnects from DFU mode and re-enumerates as a HID device. The status LED goes solid when enumeration completes.

## Verifying the HID Device

After flashing, use `hidapitester` to check communication:

```cmd
:: List HID devices (find VID 03EB PID 2402)
hidapitester --list

:: Read a heartbeat report (64 bytes)
hidapitester --vidpid 03EB:2402 --read-input 64 --length 64

:: Send LED ON
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x01 --length 64

:: Send LED OFF
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x02 --length 64

:: Send ECHO ("Hello")
hidapitester --vidpid 03EB:2402 --open --send-output 0x01,0x10,0x48,0x65,0x6C,0x6C,0x6F --length 64
```

Download: https://github.com/todbot/hidapitester/releases

## Bench Testing

Payloads under `hwtest/` are the ground-truth suite the firmware is regressed against. `test_payload.txt` is the general injection payload. `STORAGE_TEST.txt` and `ULTIMATE_TEST.txt` exercise the SD path and full opcode surface. The other files each isolate one behavior (jitter, literal delay, reflection, modifiers, and so on).

Compile a payload into `INJECT.BIN` with `tools/wrap_inject.py`, drop it onto the SD card, and reboot. `tools/inject_inspect.py` disassembles a compiled `INJECT.BIN` back to a readable trace for verification.

## Memory Map

| Region          | Start        | End          | Size    | Purpose                    |
|-----------------|--------------|--------------|---------|----------------------------|
| Flash (DFU BL)  | `0x80000000` | `0x80001FFF` | 8 KB    | Factory DFU bootloader     |
| Flash (App)     | `0x80002000` | `0x8003FFFF` | 248 KB  | Firmware (this code)       |
| SRAM            | `0x00000000` | `0x00007FFF` | 32 KB   | Stack, data, BSS           |
| USBB FIFO       | `0xFFFE0000` | `0xFFFEFFFF` | 64 KB   | USB peripheral FIFOs       |

The linker script (`src/at32uc3b1.ld`) places the application at `0x80002000`, preserving the DFU bootloader.

## Customization

### Change LED and Button Pins

In `src/main.c`:
```c
#define LED_PIN    AVR32_PIN_PB00   // adjust to your board
#define BUTTON_PIN AVR32_PIN_PB01
```

### Add HID Commands

Extend the `switch(cmd)` block in `process_hid_out_report()` in `src/main.c`.

### Change VID/PID

In `src/usb_descriptors.h`:
```c
#define USB_VENDOR_ID   0x03EB
#define USB_PRODUCT_ID  0x2402
```

## SD Card

The SD card is central to this build, not optional. `main.c` mounts it with Petit FatFs (`pf_mount`) and reads `INJECT.BIN` at boot (`src/pff.c`, `src/mmc.c`). The compiled payload lives on the card, and the host never sees the card as a drive.

## Troubleshooting

| Problem                              | Solution                                                    |
|--------------------------------------|-------------------------------------------------------------|
| "No device present"                  | Ensure the device is in DFU mode. Install WinUSB with Zadig |
| "dfu-programmer not found"           | Use `tools/dfu-programmer.exe` or add it to `PATH`          |
| LED does not light after flash       | Check pin definitions in `main.c`                           |
| HID device does not appear           | Unplug and replug. Check VID/PID in Device Manager          |
| Build error: `avr32-gcc` not found   | Add `tools/AVR Toolchain/bin/` to `PATH`                    |
| Build error: `io.h` not found        | Install the AVR32 headers alongside the toolchain           |
