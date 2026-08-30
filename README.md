# Rubberless-Ducky

Open AT32UC3B1 firmware for a DIY USB HID keystroke injector, DuckyScript-compatible. Hak5 gatekeeps the firmware for the new Rubber Ducky, so we remade it. Rubber not included. Most opcodes work; some do not yet.

On boot the firmware reads `INJECT.BIN` from the SD card into RAM and runs 16-bit big-endian words: control opcodes (DELAY, GOTO, IF, ATTACKMODE, WAIT_FOR_BUTTON_PRESS, ...) and raw HID keystrokes `(keycode << 8) | modifier`. The device is a HID keyboard only; the SD card stays hidden from the host unless you opt in.

Docs in `info/`, test payloads in `hwtest/`.

## Folder Structure

```
.
├── src/                Firmware (main.c, USB stack, descriptors, startup.S, linker, Petit FatFs, SD driver)
├── info/               Reference docs (payload language, keymap, firmware behavior)
├── hwtest/             Bench-test payloads (jitter, storage, opcode coverage, ...)
├── languages/          Keymap JSON files
├── tools/              AVR toolchain, dfu-programmer, inject_inspect.py, wrap_inject.py
├── build/              Output of `make` (firmware.elf/hex/map)
├── CONFIG.TXT
├── flash_firmware.bat  One-click: erase, flash, launch
├── Makefile
└── README.md
```

## USB Identity

VID `0x03EB` (Atmel), PID `0x2402`, HID keyboard, 64-byte reports (`0x01` OUT, `0x02` IN), 1 ms polling. Get your own VID/PID for production.

## HID Report Protocol

OUT (host to device), Report ID `0x01`: byte 1 is the command code, bytes 2 to 63 the payload.

| Command | Code   | Effect                                     |
|---------|--------|--------------------------------------------|
| NOP     | `0x00` | Ping, replies with ACK                     |
| LED ON  | `0x01` | Status LED on                              |
| LED OFF | `0x02` | Status LED off                             |
| LED TOG | `0x03` | Toggle status LED                          |
| ECHO    | `0x10` | Device echoes bytes 2 to 63 in an IN report|

IN (device to host), Report ID `0x02`: byte 1 is `0x01` alive or `0xFF` error, bytes 2 to 5 are a 4-byte LE uptime in ms, byte 6 is the button state, rest is reserved or echo. Heartbeat every 100 ms.

## Build

Needs the AVR32 GNU toolchain (`avr32-gcc`, `avr32-objcopy`, `avr32-size`). A Windows build is vendored at `tools/AVR Toolchain/`; add its `bin/` to `PATH`. Also needs Make (`choco install make`, Git for Windows, MSYS2, or WSL).

```cmd
make all
```

Output: `build/firmware.elf`, `build/firmware.hex`, `build/firmware.map`.

## Flash (Windows)

First time only: put the device in DFU mode (hold BOOT/HWB, replug), then use [Zadig](https://zadig.akeo.ie/) to bind the `AT32UC3B DFU` device to the WinUSB driver.

One-click:
```
flash_firmware.bat   (run as Administrator)
```

Manual:
```cmd
dfu-programmer at32uc3b1 erase --force
dfu-programmer at32uc3b1 flash build\firmware.hex
dfu-programmer at32uc3b1 launch
```

## Memory Map

| Region         | Range                        | Size    | Purpose                |
|----------------|------------------------------|---------|------------------------|
| Flash (DFU BL) | `0x80000000`..`0x80001FFF`   | 8 KB    | Factory DFU bootloader |
| Flash (App)    | `0x80002000`..`0x8003FFFF`   | 248 KB  | This firmware          |
| SRAM           | `0x00000000`..`0x00007FFF`   | 32 KB   | Stack, data, BSS       |
| USBB FIFO      | `0xFFFE0000`..`0xFFFEFFFF`   | 64 KB   | USB peripheral FIFOs   |

`src/at32uc3b1.ld` places the app at `0x80002000` so the DFU bootloader is preserved.

## Bench Testing

Compile a payload into `INJECT.BIN` with `tools/wrap_inject.py`, drop it on the SD card, reboot. `tools/inject_inspect.py` disassembles a `.BIN` back to a readable trace. Files in `hwtest/` are the regression suite (`test_payload.txt` is the general one; the rest each isolate one behavior).

## Verify (host)

Use [hidapitester](https://github.com/todbot/hidapitester/releases) to poke the device: `--list` to find VID `03EB` PID `2402`, `--read-input 64 --length 64` for the heartbeat, `--send-output 0x01,0x01 --length 64` for LED ON.

## Troubleshooting

- **No device present**: enter DFU mode, run Zadig
- **`dfu-programmer` not found**: use `tools/dfu-programmer.exe` or add it to `PATH`
- **`avr32-gcc` not found**: add `tools/AVR Toolchain/bin/` to `PATH`
- **LED does not light after flash**: check `LED_PIN` in `src/main.c`
- **HID device does not appear**: unplug, replug, check VID/PID in Device Manager

## Legal

Rubberless-Ducky is a clean-room reimplementation. It targets the same hardware and payload format as Hak5's USB Rubber Ducky, but is written from scratch without reference to Hak5's source, distributes no Hak5 code, ships no Hak5 firmware images, and reuses no Hak5 branding, logos, or artwork. The names "Hak5", "USB Rubber Ducky", and "DuckyScript" are trademarks of their respective owners; they are used here only to describe compatibility and interoperability, and the project name is deliberately distinct.

The firmware is provided for interoperability, education, and security research on hardware and networks the user owns or has written permission to test. It is not endorsed by, affiliated with, or supported by Hak5 LLC.

If Hak5 requests deletion of this repository, we will comply. Please open an issue on this repository, or contact the maintainers directly, and we will remove the repository or any specific content promptly on written notice. If a narrower change (renaming, removing a file, adjusting wording) would resolve the concern, we welcome that conversation first.
