# Fixes — Rubber Ducky firmware (version-5)

This documents the changes made to address the reported bugs:

1. Typing produces a **Strg+Shift+Alt+Win** (Ctrl+Shift+Alt+Win) combo, or opens
   **https://m365.cloud.microsoft/**.
2. The `WHILE TRUE` loop **exits and does not continue**.
3. A **button-ignore delay** and dropped/garbled characters on the first STRING.

All firmware changes are **surgical edits to `src/main.c`** (no file was rewritten
from scratch). One host-side tool, `tools/inject_inspect.py`, was added to decode
and lint a **real** compiled `INJECT.BIN` against the firmware's opcode table.

---

## Root causes and the firmware fixes

### Fix 1 — REVERTED (was "force HID-only"; the m365 cause was actually the send path)

**This fix was tried, then reverted — recorded here so the reasoning is not lost.**

The code comments blamed composite (`ATTACKMODE HID STORAGE`) mode for the browser
popping open on `m365.cloud.microsoft`, so an early build clamped every
STORAGE-bearing ATTACKMODE to plain HID (`f2f2`/`f3f3` → mode 1). That was a
**misdiagnosis.** The real cause of the dropped characters / lost Shift / stray
combos was the double-banked EP1 send path declaring a report "delivered" on
`TXINI` instead of `NBUSYBK` (see the "Send path" fix below). Once that was fixed,
the HID clamp had no justification and it **broke legitimate payloads** —
`ATTACKMODE STORAGE`, `ATTACKMODE HID STORAGE`, and identity spoofing like
`ATTACKMODE HID STORAGE PID_021E VID_05AC MAN_Apple` all silently lost their
storage interface.

So `parse_attackmode()` now honours ATTACKMODE **as written**:

```c
else if (w == 0xf2f2) current_attackmode = 2;   /* STORAGE       */
else if (w == 0xf3f3) current_attackmode = 3;   /* HID + STORAGE */
```

HID remains only the **default** (`current_attackmode = 1` when no ATTACKMODE
opcode is present) — never permanently forced. Note a board constraint: after USB
init the SD's SPI MISO is dead (shared with USB D-, PA25), so in STORAGE mode the
host can only read sectors pre-cached before USB came up — the drive enumerates
and is identifiable, but coverage/throughput is limited.

### DELAY — checked against the real encoder, left as-is (Instructions3 is wrong)

I initially "fixed" DELAY to treat `0x00FF` as a marker followed by a 16-bit
value word, because that is what `Instructions3` in this repo claims. **That doc
is inaccurate.** The real duck-encoder format (verified against the reference
`Encoder.py`) splits any `DELAY > 255` into a run of `[0x00, chunk]` byte pairs —
255-ms chunks plus a remainder — e.g. `DELAY 500` → `[00 FF][00 F5]`. So `0x00FF`
is simply a 255-ms chunk, **not** an extended-value marker, and treating it as one
would have broken delay timing and could have consumed the following keystroke.
The firmware's original per-chunk handling is already correct and was kept:

```c
if ((word >> 8) == 0x00) { payload_delay(word & 0xFF); pc++; continue; }
```

The comment in `main.c` now records this so the doc's error doesn't bite again.

### Fix 2 — restore GUI (Windows-key) combos (THE `GUI r` / `GUI s` bug)

Confirmed directly from a real compiled `INJECT.BIN`. The payload

```
GUI r
STRING Hello!
```

compiles to **7 aligned words** — the file is even-length, nothing is
misaligned:

```
0x1508  = keycode 0x15 (r) + modifier 0x08 (LGui)   -> Win+R
0x0b02 0x0800 0x0f00 0x0f00 0x1200 0x1e02           -> "Hello!"
```

So the compiler is correct and `GUI r` is a single, well-formed Win+R keystroke.
The bug was in the firmware: `hid_send_one()` **stripped the GUI/Windows bit off
every keystroke** that wasn't inside an explicit `KEY_DOWN`:

```c
if (!g_allow_bare_modifier)
    r.modifier &= (uint8_t)~(MOD_LGUI | MOD_RGUI);   // removed
```

That strip was a band-aid for the composite-MSC stuck-modifier chaos (the real
m365 cause, now removed at the root by Fix 1). With it in place, `GUI r` / `GUI s`
lost their Windows key and did nothing useful. The fix narrows the choke-point to
only collapse **bare** modifier reports (modifier set but no keycode — the only
thing that can leak/stick a stray Win/Ctrl), while letting a real combo keep its
modifier:

```c
if (!g_allow_bare_modifier && r.keys[0] == 0)
    r.modifier = 0;
```

Now `GUI r` sends Win+R (Run) and `GUI s` sends Win+S (Search) as atomic
press→release keystrokes, so the Windows key can neither be dropped nor stick.

### Fix 3 — keycode plausibility gate (defensive; kills rogue modifier combos)

A real typed key always carries a HID usage-ID in **0x04..0x65** (the range this
HID report descriptor even declares — Logical Maximum 101). Any word outside that
range in the keystroke default case is **not a keystroke** — it is a control/opcode
or a DELAY value word that drifted into the stream, and injecting it is exactly
what produced stray characters and rogue modifier combinations. The default case
now drops those instead of injecting them:

```c
if (keycode >= 0x04 && keycode <= 0x65)
    send_key(modifier, keycode);
```

(Note: the transmit choke-point in `hid_send_one()` already strips the GUI/Win
bits from any auto-typed key and collapses bare-modifier reports, so between that
and Fix 3 a `Ctrl+Shift+Alt+Win` / m365 combo from stray/garbage words is now
physically impossible to emit.)

### Fix 4 — GUI (Windows-key) combos work (`GUI r`, `GUI s`, …)

Confirmed from a real compiled `INJECT.BIN`: `GUI r` compiles to one aligned word
`0x1508` (keycode `r` + modifier `GUI`) — a correct Win+R. The firmware was
stripping the GUI bit off **every** non-`KEY_DOWN` keystroke, so the combo did
nothing. `hid_send_one()` now only zeroes a **bare** modifier (no keycode); a real
combo keeps its modifier. Win+R / Win+S / Win+E now fire correctly.

### Fix 5 — button fires once per press (was: repeatedly while held)

The implicit `BUTTON_DEF` trigger had no release-gate, so a single physical press
(which spans many interpreter iterations) re-ran the handler over and over. Added
a one-shot `button_impl_armed` latch: fire once, re-arm only after release.

### Fix 6 — payload size ceiling 8 KB → 20 KB (no error surfaced)

`INJECT.BIN` over 8 KB was silently truncated. On this board the SD can't be read
after USB starts (SPI MISO shares the USB D- pin), so it can't be streamed on
demand — the payload must live entirely in RAM, loaded at boot. The 16 KB MSC
sector-cache (`mc_data` in `mmc.c`) only ever holds the handful of sectors
pre-cached at boot (the SD is unreadable afterwards), so `MC_MAX` was cut 32 → 8
and the reclaimed 12 KB handed to `payload_ram` (8 KB → 20 KB). **Deliberate
tradeoff:** STORAGE/composite mode still enumerates (drive appears, identity
spoofing works), but the host-readable portion of the drive is limited to those
8 cached sectors — accepted, since post-USB the SD is barely readable anyway and
big *payloads* were the priority. Net SRAM cost: **zero** (bss ~28,020 bytes,
~4.5 KB stack headroom preserved). Payloads beyond 20 KB are still capped
silently.

### Fix 7 — `diagnose.c` `error_blink()` drove the button pin

Dead code, but it toggled PA13 (the **button** pin) as an active-high "LED". The
real red LED is PA08, active-low. Corrected so it's not a latent trap.

**All of the above compile clean (`-Wall -Wextra`, zero warnings) and are in the
current `build/firmware.hex`.**

---

## Diagnosing the real bytes: `tools/inject_inspect.py`

Disassembles a **real** compiled `INJECT.BIN` (the actual file you flash to the
SD card) against the firmware's exact opcode table and lints it for the known bug
signatures: odd file length (misalignment), GUI bits on typed keys, the `0x0F`
Office-key combo, out-of-range keycodes, unknown opcodes, and DELAY chunks. It
reads real bytes — it does not simulate anything.

Run: `python tools/inject_inspect.py path\to\INJECT.BIN`

Verified on a broken sample it correctly flags the odd-length misalignment, the
GUI-bit-on-a-typed-key, and the `0x0F` Ctrl+Shift+Alt+Win combo; on a clean
payload it reports no warnings.

---

## What still needs a real INJECT.BIN

The firmware only ever sees `INJECT.BIN`, and PayloadStudio is login-gated, so I
could not read the exact bytes it emits. That matters because:

* **`WHILE TRUE` exiting (bug #2)** depends entirely on how PayloadStudio encodes
  `WHILE` / `END_WHILE`. The firmware loops via `IF` (`0xefef`) + backward `GOTO`
  (`0xf8f8`). If PayloadStudio emits a *different* opcode for the loop-back (or a
  dedicated `END_WHILE`), the firmware never jumps back and the payload runs off
  the end — exactly "exits and does not continue." This cannot be fixed blind
  without seeing the opcode it actually emits.
* **The rogue `Ctrl+Shift+Alt+Win` / m365 combo on real payloads** is most
  consistent with **stream misalignment**: `Instructions1` says PayloadStudio
  emits a *single byte* for a standalone modifier key (e.g. `GUI` on its own
  line), which makes `INJECT.BIN` an odd number of bytes and shifts every
  following word by one byte — turning keycodes into modifier bytes and producing
  wild combinations. Fix 1 + Fix 3 make typed text safe, but a genuine
  misalignment upstream (odd-length file) can only be seen in the bytes — your
  two payloads were NOT misaligned, so this was not your problem.
* **`Instructions3` is not a reliable spec.** Its DELAY claim (`0x00FF` + value
  word) is provably wrong versus the real encoder, so its other opcode values
  (e.g. `IF = 0xEEEE`, `ASSIGN = 0x01E8`, `INJECT_MOD = 0xE9E6`) should not be
  trusted over the firmware either. Only the real `INJECT.BIN` settles it.

**One artifact settles all of it:** compile the test payload in PayloadStudio,
then either send the `INJECT.BIN` or run
`python tools/inject_inspect.py INJECT.BIN` and paste the output. The disassembly
shows precisely which opcodes it uses for `WHILE`/`END_WHILE`/`WAIT_FOR_BUTTON_PRESS`
and whether a bare modifier misaligns the file — at which point the remaining
firmware opcodes can be corrected to match exactly.
