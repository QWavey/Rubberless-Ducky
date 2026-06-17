/**
 * @file main.c
 * @brief AT32UC3B1 USB Rubber Ducky firmware — clean rewrite.
 *
 * Reads INJECT.BIN from SD into RAM and interprets it as a sequence of
 * 16-bit big-endian words.  Each word is either an opcode (control flow,
 * DELAY, ATTACKMODE, …) or a raw HID keystroke encoded as
 *
 *     (keycode << 8) | modifier
 *
 * USB profile is forced to HID-only (no mass-storage interface on the host
 * side) — the SD card is read by firmware over SPI, so the host never
 * mounts a drive letter and Windows AutoPlay never fires.
 *
 * Reliability guarantees:
 *   - Every keystroke is press → drain → release → drain.  The host cannot
 *     miss a release, so a modifier (Shift/Ctrl/Alt/GUI) can never get
 *     "stuck" between characters.
 *   - At every dangerous boundary (payload start, before+after a button
 *     press) the firmware sends THREE back-to-back zero reports.  Three
 *     reports across three host poll cycles cannot all be lost.
 *   - The physical button is latched in software: presses that happen
 *     while a STRING is typing are remembered and consumed when
 *     WAIT_FOR_BUTTON_PRESS is reached — the user no longer has to hold
 *     the button until typing finishes.
 *   - bInterval is 1 ms in the endpoint descriptor (see usb_descriptors.c)
 *     so typing runs at host poll speed, not 10× slower.
 */

#include <stdint.h>
#include <avr32/io.h>
#include <string.h>
#include "usb_hid.h"
#include "usb_descriptors.h"
#include "pff.h"
#include "diskio.h"

/* ---------- freestanding libc ------------------------------------------- */
void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest; const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}
void *memset(void *s, int c, size_t n) {
    char *p = s;
    while (n--) *p++ = (char)c;
    return s;
}

#define Enable_global_interrupt()  __asm__ __volatile__ ("csrf 16")
#define Disable_global_interrupt() __asm__ __volatile__ ("ssrf 16")

#define CYCLES_PER_MS  48000u   /* 48 MHz core */

/* ---------- GPIO pin map ----------------------------------------------- */
#define LED_PORT_GREEN 0
#define LED_PIN_GREEN  7   /* PA07 green LED, active-low */
#define LED_PORT_RED   0
#define LED_PIN_RED    8   /* PA08 red   LED, active-low */
#define BTN_PORT       0
#define BTN_PIN        13  /* PA13 button, pull-up, active-low */

#define _GPIO_PORT(n)  ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void gpio_out      (int p, int b){volatile avr32_gpio_port_t*g=_GPIO_PORT(p);g->gpers=1u<<b;g->oders=1u<<b;}
static inline void gpio_in_pullup(int p, int b){volatile avr32_gpio_port_t*g=_GPIO_PORT(p);g->gpers=1u<<b;g->oderc=1u<<b;g->puers=1u<<b;}
static inline void gpio_high(int p, int b){_GPIO_PORT(p)->ovrs=1u<<b;}
static inline void gpio_low (int p, int b){_GPIO_PORT(p)->ovrc=1u<<b;}
static inline bool gpio_read(int p, int b){return !!(_GPIO_PORT(p)->pvr & (1u<<b));}

/* ---------- HID report layout (matches usb_hid_report_descriptor) ------ */
typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} keyboard_report_t;

/* Modifier bit masks (low byte of each keystroke word). */
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

/* ---------- USB ready / state ------------------------------------------ */
static volatile bool g_usb_ready    = false;
static bool          g_sd_card_ok   = false;
static bool          g_payload_run  = false;
void usb_device_enumerated_cb(void)       { g_usb_ready = true; }
void usb_device_config_requested_cb(void) {}

/* Timestamp (CPU cycle count) of the most recent mass-storage activity.
 * usb_msc.c calls storage_activity_mark() on every command and every block
 * transfer; main() uses it to wait for the Windows SD-mount storm to settle
 * before it starts typing, so the first STRING isn't shredded by the mount. */
static volatile uint32_t g_last_msc_cyc = 0;
void storage_activity_mark(void)          { g_last_msc_cyc = __builtin_mfsr(AVR32_COUNT); }

/* ---------- Variables / state shared with the payload ------------------ */
static uint16_t variables[1024];
static uint16_t button_enabled     = 1;
static uint16_t button_user_defined= 0;
static uint16_t system_leds_enabled= 1;
static uint16_t caps_lock_on       = 0;
static uint16_t num_lock_on        = 0;
static uint16_t scroll_lock_on     = 0;
static uint16_t saved_caps_lock_on = 0;
static uint16_t saved_num_lock_on  = 0;
static uint16_t saved_scroll_on    = 0;
static uint16_t button_push_received = 0;
static volatile uint16_t button_pending = 0;

uint16_t        current_vid        = 0x05AC;
uint16_t        current_pid        = 0x021E;
uint8_t         current_attackmode = 1;   /* always HID-only (see force below) */
static uint16_t saved_attackmode   = 1;
static uint16_t saved_vid          = 0x05AC;
static uint16_t saved_pid          = 0x021E;

static uint16_t call_stack[32];
static uint8_t  call_stack_ptr   = 0;
static uint16_t button_def_pc    = 0;
static bool     in_button_handler= false;

/* ---------- Cycle counter / delay -------------------------------------- */
static inline uint32_t cyc(void) { return __builtin_mfsr(AVR32_COUNT); }

/* Plain delay used inside non-typing windows.  Pumps the USB stack so
 * EP0 control transfers (SET_CONFIGURATION, GET_DESCRIPTOR, etc.) still
 * complete during the wait. */
static void delay_ms(uint32_t ms) {
    while (ms--) {
        uint32_t t0 = cyc();
        while ((cyc() - t0) < CYCLES_PER_MS) usb_device_task();
    }
}

/* ---------- LED helpers ------------------------------------------------ */
static inline void led_green(void){ if(!system_leds_enabled)return; gpio_low(LED_PORT_GREEN,LED_PIN_GREEN); gpio_high(LED_PORT_RED,LED_PIN_RED); }
static inline void led_red  (void){ if(!system_leds_enabled)return; gpio_high(LED_PORT_GREEN,LED_PIN_GREEN); gpio_low(LED_PORT_RED,LED_PIN_RED); }
static inline void led_off  (void){ gpio_high(LED_PORT_GREEN,LED_PIN_GREEN); gpio_high(LED_PORT_RED,LED_PIN_RED); }

static uint32_t blink_t0  = 0;
static bool     blink_on  = false;
static void blink_tick(void) {
    if (!system_leds_enabled) return;
    if ((cyc() - blink_t0) < CYCLES_PER_MS * 250u) return;
    blink_t0 = cyc();
    blink_on = !blink_on;
    if (blink_on) gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
    else          gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high(LED_PORT_RED, LED_PIN_RED);
}

/* ---------- Button latch (poll from inside loops) ---------------------- */
static inline void poll_button(void) {
    if (!button_enabled || button_pending) return;
    if (!gpio_read(BTN_PORT, BTN_PIN)) {
        button_pending      = 1;
        button_push_received= 1;
    }
}

/* ---------- HID Out (LED) report callback ------------------------------ */
void usb_hid_report_out_cb(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    uint8_t leds   = data[0];
    num_lock_on    = !!(leds & 0x01);
    caps_lock_on   = !!(leds & 0x02);
    scroll_lock_on = !!(leds & 0x04);
}

/* ======================================================================
 * HID transmit core
 *
 * Single-bank EP1-IN: TXINI=1 means the previous report was actually
 * collected by the host.  We use that as our "drained" signal and never
 * proceed until we see it (so a release can NEVER be silently skipped —
 * that was the bug behind every reported "stuck modifier" symptom).
 * ====================================================================== */
#define TX_TIMEOUT_MS 1000u

/* When false (the default), the transmit choke-point refuses to send a
 * "modifier-only" report (a non-zero modifier with NO keycode).  A keycode
 * detector proved the device was flooding bare modifier reports after the
 * button press — the modifier byte cycling through every bit including BOTH
 * Windows keys (0x08 LGUI / 0x80 RGUI), which is exactly what launched the
 * browser to m365.cloud.microsoft.  Normal typing ALWAYS carries a keycode,
 * so collapsing any bare-modifier report to a clean release here makes that
 * flood physically impossible to emit, whatever its source.
 *
 * The flag is raised ONLY around an explicit KEY_DOWN opcode, where holding
 * a bare modifier (e.g. CTRL for a combo) is intentional. */
static bool g_allow_bare_modifier = false;

static void hid_send_one(const keyboard_report_t *src) {
    keyboard_report_t r = *src;

    /* ---- Safety choke-point ---------------------------------------- */
    /* Never let the Windows (GUI) modifier go out as part of auto-typed
     * text — a Ducky STRING never needs it, and a stray GUI bit is what
     * opens Start / launches apps / lands on m365. */
    if (!g_allow_bare_modifier)
        r.modifier &= (uint8_t)~(MOD_LGUI | MOD_RGUI);

    /* If after that the report is "modifier(s) held but no key", and we're
     * not in an intentional KEY_DOWN hold, turn it into a pure release so a
     * bare Alt/Ctrl/Shift/Win tap can never reach the host. */
    if (!g_allow_bare_modifier && r.keys[0] == 0 && r.modifier != 0)
        r.modifier = 0;

    uint32_t overall = cyc();

    /* ---- Phase 1: actually QUEUE the report ------------------------------
     * The old code waited for the bank to be free, then called
     * usb_hid_send_report() and IGNORED its return value.  usb_hid_send_report
     * re-checks readiness and returns false (sending NOTHING) if the bank
     * isn't free at that instant — so a press could silently vanish while the
     * following release sailed through, dropping exactly one character (e.g.
     * "Make" → "ake").  We now loop until the report is genuinely accepted. */
    for (;;) {
        if (usb_hid_in_endpoint_ready() &&
            usb_hid_send_report((const uint8_t*)&r, sizeof(r))) {
            break;  /* report is in the bank */
        }
        if ((cyc() - overall) > CYCLES_PER_MS * TX_TIMEOUT_MS) return; /* anti-hang only */
        usb_device_task();
        poll_button();
    }

    /* ---- Phase 2: wait until the host has collected it (bank free again) -- */
    uint32_t t0 = cyc();
    while (!usb_hid_in_endpoint_ready()) {
        if ((cyc() - t0) > CYCLES_PER_MS * TX_TIMEOUT_MS) return;
        usb_device_task();
        poll_button();
    }
}

/* Send one keypress.  ALWAYS paired with an immediate release via the
 * caller's send_key() — no caller is allowed to send a press without a
 * matching release. */
static inline void hid_send_report(uint8_t modifier, uint8_t keycode) {
    keyboard_report_t r;
    memset(&r, 0, sizeof(r));
    r.modifier = modifier;
    r.keys[0]  = keycode;
    hid_send_one(&r);
}

static inline void hid_release_all(void) {
    keyboard_report_t z;
    memset(&z, 0, sizeof(z));
    hid_send_one(&z);
}

/* Hard scrub: N back-to-back release-all reports.  Used at payload start
 * and at every WAIT_FOR_BUTTON_PRESS boundary.  Three drained zero
 * reports cannot all be lost — host modifier state is guaranteed empty
 * afterwards. */
static void hid_scrub(int n) {
    for (int i = 0; i < n; i++) hid_release_all();
}

/* Warm up the host's HID polling before typing resumes after an idle/storage
 * window.
 *
 * Root cause of the "first few characters of each STRING drop" symptom: while
 * the SD-card mount runs in an idle window (DELAY / WAIT_FOR_BUTTON_PRESS),
 * Windows de-prioritises the keyboard IN endpoint.  When the next STRING
 * starts, the host is still ramping HID polling back up, so the opening
 * keystrokes land in a dead zone and are lost.  A blind 3-report scrub doesn't
 * fix it because it can't tell whether the host is actually polling yet.
 *
 * This routine sends release-all (zero) reports and measures how fast each one
 * drains.  A drain faster than ~one bInterval means the host is polling EP1 at
 * full rate.  We require several FAST drains in a row before returning, so by
 * the time real keystrokes start the host is fully warmed up and nothing drops.
 * Also doubles as a modifier scrub (all-zero).  Bounded so a non-polling host
 * can't hang us. */
static void hid_warmup(void) {
    keyboard_report_t z;
    memset(&z, 0, sizeof(z));
    int consecutive_fast = 0;
    uint32_t overall = cyc();

    while (consecutive_fast < 8) {
        if ((cyc() - overall) > CYCLES_PER_MS * 1500u) return; /* failsafe */

        while (!usb_hid_in_endpoint_ready()) {
            usb_device_task();
            if ((cyc() - overall) > CYCLES_PER_MS * 1500u) return;
        }
        usb_hid_send_report((uint8_t*)&z, sizeof(z));

        uint32_t t0 = cyc();
        while (!usb_hid_in_endpoint_ready()) {
            usb_device_task();
            if ((cyc() - overall) > CYCLES_PER_MS * 1500u) return;
        }
        /* Drain under ~4 ms → host is polling at the 1 ms full rate. */
        if ((cyc() - t0) < CYCLES_PER_MS * 4u) consecutive_fast++;
        else                                   consecutive_fast = 0;
    }
}

/* Inter-event settle (ms).  Even though hid_send_one waits for the host to
 * drain each report at the USB level, the host's *input stack* (the part that
 * turns HID reports into characters in the focused app) needs a little spacing
 * or it coalesces/reorders fast back-to-back press+release pairs — which shows
 * up as wrong characters and stray digits, not just dropped ones.  5 ms is the
 * smallest value that typed cleanly in testing; 0 ms garbled. */
/* Per-key spacing between a press and its release.  This is what keeps the
 * host from coalescing the pair (which drops the key), so it can't go too low.
 * 4 ms is the steady-state value that types cleanly once the SD is mounted. */
static uint16_t char_settle_ms = 4;

/* Extra spacing while the SD mount is still busy: the host reads the keyboard
 * in bursts then, so press/release need to be further apart to be seen
 * separately.  Applied only until the mount goes quiet. */
#define MOUNT_BUSY_EXTRA_MS 8u

/* Latches true once storage has been quiet for a spell == mount finished.
 * After that, typing runs at full speed with only a storage trickle. */
static bool g_mount_done = false;

static void settle(uint32_t ms) {
    while (ms--) {
        uint32_t t0 = cyc();
        while ((cyc() - t0) < CYCLES_PER_MS) usb_device_task();
        poll_button();
    }
}

/* Send a key as a complete (press → settle → release → settle) operation.
 *
 * Storage is suppressed ONLY for the press→release pair (so a storage tick can
 * never land between a key's press and its release and leave a modifier stuck —
 * that was the Ctrl-stuck → Ctrl+T → m365 flood), and re-enabled for the
 * inter-key gap so the SD mount makes progress BETWEEN keystrokes.  That gives
 * Hak5-style concurrency: the card mounts in the background WHILE the first
 * STRING types, instead of us stalling for the mount up front. */
static inline void send_key(uint8_t modifier, uint8_t keycode) {
    /* Latch "mount finished" once storage has gone quiet for a moment. */
    if (!g_mount_done && (cyc() - g_last_msc_cyc) > CYCLES_PER_MS * 300u)
        g_mount_done = true;

    /* Spacing: tight once mounted, wider while the mount is busy so the host
     * (reading in bursts then) sees press and release separately.  Storage is
     * off across the press→release pair so an SD read can't split a key; in
     * the inter-key gap it runs full-speed while mounting, then trickles. */
    uint32_t hold = g_mount_done ? char_settle_ms
                                 : (uint32_t)char_settle_ms + MOUNT_BUSY_EXTRA_MS;

    usb_msc_set_budget(0);
    hid_send_report(modifier, keycode);
    settle(hold);
    hid_release_all();
    usb_msc_set_budget(g_mount_done ? 1 : -1);
    settle(hold);
    usb_msc_set_budget(0);
}

/* Pump USB while idle (keeps the host's HID driver polling). */
static void usb_keepalive(void) {
    static uint32_t last = 0;
    usb_device_task();
    if ((cyc() - last) >= CYCLES_PER_MS * 8u) {
        if (usb_hid_in_endpoint_ready()) {
            keyboard_report_t z; memset(&z, 0, sizeof(z));
            usb_hid_send_report((uint8_t*)&z, sizeof(z));
        }
        last = cyc();
    }
}

/* DELAY opcode handler: idle for `ms`, keeping HID warm AND letting the SD
 * card stay serviced so Windows doesn't drop the drive.
 *
 * Storage is ENABLED for the delay (drive stays alive), then SUPPRESSED again,
 * and finally we hid_scrub() to flush the keyboard state.  That last scrub is
 * the crucial fix: while MSC was being serviced a keyboard release could be
 * delayed and leave a modifier (Ctrl/Alt) stuck on the host; the next STRING
 * would then type as Ctrl+letter (Ctrl+T → new browser tab → m365, plus the
 * Ctrl+C/V/S the detector caught).  Scrubbing with MSC already suppressed
 * guarantees the modifier state is clean before typing resumes. */
static void payload_delay(uint32_t ms) {
    /* Idle window: storage gets UNLIMITED budget so the SD mount finishes as
     * fast as possible while we're not typing. */
    usb_msc_set_budget(-1);
    while (ms--) {
        uint32_t t0 = cyc();
        while ((cyc() - t0) < CYCLES_PER_MS) usb_keepalive();
        poll_button();
    }

    usb_msc_set_budget(0);   /* quiet storage; trickle resumes via send_key */
    hid_warmup();
}

/* Wait until the host has actually started polling EP1 IN.  Send one
 * invisible empty report; the moment the bank is free again the host
 * has serviced an IN token = HID driver is up. */
static void wait_for_host_polling(void) {
    keyboard_report_t z; memset(&z, 0, sizeof(z));
    uint32_t deadline = cyc() + CYCLES_PER_MS * 10000u;
    for (;;) {
        while (!usb_hid_in_endpoint_ready()) {
            if ((int32_t)(cyc() - deadline) > 0) return;
            usb_device_task();
        }
        usb_hid_send_report((uint8_t*)&z, sizeof(z));
        uint32_t t0 = cyc();
        while ((cyc() - t0) < CYCLES_PER_MS * 250u) {
            usb_device_task();
            if (usb_hid_in_endpoint_ready()) return;
            if ((int32_t)(cyc() - deadline) > 0) return;
        }
    }
}

/* ======================================================================
 * Printable-ASCII → HID keycode (used by INJECT_VAR opcodes for non-
 * literal characters; the bulk of typing goes through raw key bytes
 * in the bytecode default-case).
 * ====================================================================== */
typedef struct { uint8_t keycode, modifier; } KeyEntry;

static KeyEntry char_to_hid(char c) {
    KeyEntry e = {0, 0};
    if (c >= 'a' && c <= 'z') { e.keycode = 0x04 + (uint8_t)(c-'a'); return e; }
    if (c >= 'A' && c <= 'Z') { e.keycode = 0x04 + (uint8_t)(c-'A'); e.modifier = MOD_LSHIFT; return e; }
    if (c >= '1' && c <= '9') { e.keycode = 0x1E + (uint8_t)(c-'1'); return e; }
    switch (c) {
        case '0':  e.keycode = 0x27; return e;
        case '\n': case '\r': e.keycode = 0x28; return e;
        case '\t': e.keycode = 0x2B; return e;
        case ' ':  e.keycode = 0x2C; return e;
        case '-':  e.keycode = 0x2D; return e;
        case '=':  e.keycode = 0x2E; return e;
        case '[':  e.keycode = 0x2F; return e;
        case ']':  e.keycode = 0x30; return e;
        case '\\': e.keycode = 0x31; return e;
        case ';':  e.keycode = 0x33; return e;
        case '\'': e.keycode = 0x34; return e;
        case '`':  e.keycode = 0x35; return e;
        case ',':  e.keycode = 0x36; return e;
        case '.':  e.keycode = 0x37; return e;
        case '/':  e.keycode = 0x38; return e;
        case '!':  e.keycode = 0x1E; e.modifier = MOD_LSHIFT; return e;
        case '@':  e.keycode = 0x1F; e.modifier = MOD_LSHIFT; return e;
        case '#':  e.keycode = 0x20; e.modifier = MOD_LSHIFT; return e;
        case '$':  e.keycode = 0x21; e.modifier = MOD_LSHIFT; return e;
        case '%':  e.keycode = 0x22; e.modifier = MOD_LSHIFT; return e;
        case '^':  e.keycode = 0x23; e.modifier = MOD_LSHIFT; return e;
        case '&':  e.keycode = 0x24; e.modifier = MOD_LSHIFT; return e;
        case '*':  e.keycode = 0x25; e.modifier = MOD_LSHIFT; return e;
        case '(':  e.keycode = 0x26; e.modifier = MOD_LSHIFT; return e;
        case ')':  e.keycode = 0x27; e.modifier = MOD_LSHIFT; return e;
        case '_':  e.keycode = 0x2D; e.modifier = MOD_LSHIFT; return e;
        case '+':  e.keycode = 0x2E; e.modifier = MOD_LSHIFT; return e;
        case '{':  e.keycode = 0x2F; e.modifier = MOD_LSHIFT; return e;
        case '}':  e.keycode = 0x30; e.modifier = MOD_LSHIFT; return e;
        case '|':  e.keycode = 0x31; e.modifier = MOD_LSHIFT; return e;
        case ':':  e.keycode = 0x33; e.modifier = MOD_LSHIFT; return e;
        case '"':  e.keycode = 0x34; e.modifier = MOD_LSHIFT; return e;
        case '~':  e.keycode = 0x35; e.modifier = MOD_LSHIFT; return e;
        case '<':  e.keycode = 0x36; e.modifier = MOD_LSHIFT; return e;
        case '>':  e.keycode = 0x37; e.modifier = MOD_LSHIFT; return e;
        case '?':  e.keycode = 0x38; e.modifier = MOD_LSHIFT; return e;
        default:   return e;
    }
}

static void type_char(char c) {
    KeyEntry e = char_to_hid(c);
    if (e.keycode == 0) return;
    send_key(e.modifier, e.keycode);
}

static void type_number(int n) {
    if (n < 0) { type_char('-'); n = -n; }
    if (n == 0) { type_char('0'); return; }
    char buf[7]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) type_char(buf[i]);
}

/* ======================================================================
 * Payload storage and word access
 * ====================================================================== */
static uint8_t payload_ram[8192];
static uint16_t g_word_count = 0;   /* number of valid words in payload_ram */

/* Bounds-safe word fetch.  Anything at or past the end of the loaded payload
 * reads as 0x0000 (a NOP), so a bad GOTO target or an over-run scan can never
 * make the interpreter execute stale RAM as keystrokes. */
static inline uint16_t get_word(uint16_t idx) {
    if (idx >= g_word_count) return 0;
    uint16_t b = (uint16_t)(idx * 2);
    return ((uint16_t)payload_ram[b] << 8) | payload_ram[b + 1];
}

/* ---------- read/write to the variable-store --------------------------- */
static uint32_t rand_seed = 12345;
static uint16_t rand_range(uint16_t lo, uint16_t hi) {
    rand_seed = rand_seed * 1103515245u + 12345u;
    if (hi <= lo) return lo;
    return lo + (uint16_t)((rand_seed >> 16) % ((uint32_t)(hi - lo + 1)));
}
static uint16_t rand_min = 0, rand_max = 9;

static uint16_t read_var(uint16_t a) {
    if (a >= 0x0001 && a < 1024) return variables[a];
    switch (a) {
        case 0x8042: return button_enabled;
        case 0x8142: return button_user_defined;
        case 0x8442: return button_push_received;
        case 0x9042: return caps_lock_on;
        case 0x9142: return num_lock_on;
        case 0x9242: return scroll_lock_on;
        case 0x9342: return saved_caps_lock_on;
        case 0x9442: return saved_num_lock_on;
        case 0x9542: return saved_scroll_on;
        case 0x9B42: return current_vid;
        case 0x9C42: return current_pid;
        case 0x9D42: return 1;             /* OS = WINDOWS */
        case 0xA042: return current_attackmode;
        case 0xf042: return rand_min;
        case 0xf142: return rand_max;
        case 0xf242: return rand_range(rand_min, rand_max);
        case 0x6742: return 0;             /* FALSE */
        case 0x6842: return 1;             /* TRUE */
        case 0x6942: return 1;             /* WINDOWS */
        case 0x7042: return 2;             /* MACOS */
        case 0x7142: return 3;             /* LINUX */
        case 0x7242: return 4;             /* ANDROID */
        case 0x7342: return 5;             /* IOS */
        case 0x7442: return 6;             /* CHROMEOS */
        default:     return 0;
    }
}
static void write_var(uint16_t a, uint16_t v) {
    if (a >= 0x0001 && a < 1024) { variables[a] = v; return; }
    switch (a) {
        case 0x8042: button_enabled       = v; break;
        case 0x8142: button_user_defined  = v; break;
        case 0x8442: button_push_received = v; break;
        case 0x8642: system_leds_enabled  = v; break;
        case 0x9042: caps_lock_on         = v; break;
        case 0x9142: num_lock_on          = v; break;
        case 0x9242: scroll_lock_on       = v; break;
        case 0x9342: saved_caps_lock_on   = v; break;
        case 0x9442: saved_num_lock_on    = v; break;
        case 0x9542: saved_scroll_on      = v; break;
        case 0x9B42: current_vid          = v; break;
        case 0x9C42: current_pid          = v; break;
        case 0xA042: current_attackmode   = (uint8_t)v; break;
        case 0xf042: rand_min             = v; break;
        case 0xf142: rand_max             = v; break;
    }
}

static uint16_t eval_op(uint16_t op, uint16_t a, uint16_t b) {
    switch (op) {
        case 0xe802: return a + b;
        case 0xe803: return a - b;
        case 0xe804: return a * b;
        case 0xe805: return b ? a / b : 0;
        case 0xe806: return a == b;
        case 0xe807: return a != b;
        case 0xe808: return a <  b;
        case 0xe809: return a >  b;
        case 0xe8a8: return a <= b;
        case 0xe8a9: return a >= b;
        case 0xe8aa: return a && b;
        case 0xe8bb: return a || b;
        case 0xe80a: return a & b;
        case 0xe80b: return a | b;
        case 0xe80c: return a >> b;
        case 0xe80d: return a << b;
        case 0xe80e: return b ? a % b : 0;
        case 0x0fe8: { uint16_t r=1; for (uint16_t i=0; i<b; i++) r*=a; return r; }
        default: return 0;
    }
}

/* ======================================================================
 * USB re-enumeration (apply_attackmode)
 *
 * In this build the host-side profile is ALWAYS HID-only.  Composite
 * (HID + Mass Storage) was the source of the "Windows opens m365 via
 * AutoPlay during the WAIT_FOR_BUTTON_PRESS window" symptom — the SD
 * card is still read internally by firmware, the host just never sees a
 * drive letter.  We honour VID/PID/MAN/PROD/SERIAL changes from the
 * payload but ignore the STORAGE bit of ATTACKMODE.
 * ====================================================================== */
FATFS fs;
static uint16_t last_vid  = 0xFFFF;
static uint16_t last_pid  = 0xFFFF;
static uint8_t  last_mode = 0xFF;

static void apply_attackmode(void) {
    if (current_attackmode == last_mode && current_vid == last_vid && current_pid == last_pid) return;

    g_usb_ready = false;
    usb_device_disable();
    delay_ms(250);

    extern const uint8_t  usb_config_descriptor_hid[];
    extern const uint16_t usb_config_descriptor_hid_size;
    extern const uint8_t  usb_config_descriptor_msc[];
    extern const uint16_t usb_config_descriptor_msc_size;
    extern const uint8_t  usb_config_descriptor_comp[];
    extern const uint16_t usb_config_descriptor_comp_size;
    extern const uint8_t *usb_config_descriptor;
    extern uint16_t       usb_config_descriptor_size;
    extern uint8_t        usb_device_descriptor[];

    if (current_attackmode == 2) {            /* STORAGE only */
        usb_config_descriptor      = usb_config_descriptor_msc;
        usb_config_descriptor_size = usb_config_descriptor_msc_size;
    } else if (current_attackmode == 3) {     /* HID + STORAGE (composite) */
        usb_config_descriptor      = usb_config_descriptor_comp;
        usb_config_descriptor_size = usb_config_descriptor_comp_size;
    } else {                                   /* 0=OFF or 1=HID */
        usb_config_descriptor      = usb_config_descriptor_hid;
        usb_config_descriptor_size = usb_config_descriptor_hid_size;
    }

    /* Disambiguate the PID for composite/storage when using the default Hak5
     * VID/PID, so Windows keeps separate driver state per mode. */
    uint16_t pid = current_pid;
    if (current_vid == 0x05AC && current_pid == 0x021E) {
        if      (current_attackmode == 3) pid ^= 0x4242;
        else if (current_attackmode == 2) pid ^= 0x1337;
    }

    usb_device_descriptor[8]  = (uint8_t)(current_vid & 0xFF);
    usb_device_descriptor[9]  = (uint8_t)(current_vid >> 8);
    usb_device_descriptor[10] = (uint8_t)(pid & 0xFF);
    usb_device_descriptor[11] = (uint8_t)(pid >> 8);

    if (current_attackmode != 0) {
        usb_device_enable();
        uint32_t timeout = 5000;
        while (!g_usb_ready && timeout--) {
            uint32_t t0 = cyc();
            while ((cyc() - t0) < CYCLES_PER_MS) usb_device_task();
        }
    }
    last_mode = current_attackmode;
    last_vid  = current_vid;
    last_pid  = current_pid;
}

static void parse_attackmode(uint16_t *pcp, uint16_t wc) {
    uint16_t pc = *pcp;
    uint16_t w  = get_word(pc);
    if      (w == 0xf0f0) current_attackmode = 0;
    else if (w == 0xf1f1) current_attackmode = 1;
    else if (w == 0xf2f2) current_attackmode = 2;
    else if (w == 0xf3f3) current_attackmode = 3;
    else                  return;
    pc++;
    while (pc < wc && get_word(pc) != w) {
        uint16_t p = get_word(pc);
        if      (p == 0xf5f5) { current_vid = read_var(get_word(pc+1)); pc += 2; }
        else if (p == 0xf6f6) { current_pid = read_var(get_word(pc+1)); pc += 2; }
        else if (p == 0xf9f9) {
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xf9f9) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_manufacturer_descriptor, buf, 32);
        }
        else if (p == 0xfafa) {
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xfafa) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_product_descriptor, buf, 32);
        }
        else if (p == 0xfbfb) {
            pc++;
            char buf[13]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xfbfb) {
                if (bi < 12) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_serial_descriptor, buf, 12);
            usb_custom_serial_set = 1;
        }
        else pc++;
    }
    if (pc < wc) pc++;
    *pcp = pc;
}

static void start_usb_and_wait(void) {
    usb_device_init();
    sd_mark_spi_dead();
    usb_device_register_callback(USB_EVENT_ENUMERATED,       usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);
    apply_attackmode();
}

/* ======================================================================
 * Boot + interpreter
 * ====================================================================== */
int main(void)
{
    Disable_global_interrupt();

    /* --- Disable watchdog --- */
    volatile uint32_t *wdt = (volatile uint32_t *)(&AVR32_WDT.ctrl);
    uint32_t wv = *wdt & ~AVR32_WDT_CTRL_KEY_MASK;
    *wdt = wv | (0x55ul << AVR32_WDT_CTRL_KEY_OFFSET);
    *wdt = (wv & ~AVR32_WDT_CTRL_EN_MASK) | (0xAAul << AVR32_WDT_CTRL_KEY_OFFSET);

    /* --- Clock: OSC0 → PLL0 → 48 MHz --- */
    AVR32_PM.mcctrl = 0;
    AVR32_PM.OSCCTRL0.startup = AVR32_PM_OSCCTRL0_STARTUP_2048_RCOSC;
    AVR32_PM.OSCCTRL0.mode    = AVR32_PM_OSCCTRL0_MODE_CRYSTAL_G3;
    AVR32_PM.mcctrl = AVR32_PM_MCCTRL_OSC0EN_MASK;
    while (!(AVR32_PM.poscsr & AVR32_PM_POSCSR_OSC0RDY_MASK));

    AVR32_PM.pll[0] = 0;
    AVR32_PM.pll[0] = (0 << AVR32_PM_PLLOSC_OFFSET)
                    | (1 << AVR32_PM_PLLDIV_OFFSET)
                    | (7 << AVR32_PM_PLLMUL_OFFSET)
                    | (3 << AVR32_PM_PLLOPT_OFFSET)
                    | (63 << AVR32_PM_PLLCOUNT_OFFSET);
    AVR32_PM.pll[0] |= AVR32_PM_PLLEN_MASK;
    while (!(AVR32_PM.poscsr & AVR32_PM_POSCSR_LOCK0_MASK));

    AVR32_FLASHC.fcr = (1 << AVR32_FLASHC_FCR_FWS_OFFSET);
    AVR32_PM.mcctrl  = AVR32_PM_MCCTRL_OSC0EN_MASK
                     | (AVR32_PM_MCCTRL_MCSEL_PLL0 << AVR32_PM_MCCTRL_MCSEL_OFFSET);

    /* --- GPIO: LEDs off, button input pull-up --- */
    gpio_out      (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high     (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_out      (LED_PORT_RED,   LED_PIN_RED);
    gpio_high     (LED_PORT_RED,   LED_PIN_RED);
    gpio_in_pullup(BTN_PORT,       BTN_PIN);

    /* --- Mount SD / open INJECT.BIN ----------------------------------- */
    g_sd_card_ok = false;
    if (pf_mount(&fs) != FR_OK) {
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) usb_device_task();
    }
    if (pf_open("INJECT.BIN") != FR_OK) {
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) usb_device_task();
    }
    g_sd_card_ok = true;
    gpio_low(LED_PORT_GREEN, LED_PIN_GREEN);

    UINT br = 0;
    if (pf_read(payload_ram, sizeof(payload_ram), &br) != FR_OK || br == 0) {
        g_sd_card_ok = false;
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) usb_device_task();
    }
    uint16_t word_count = (uint16_t)(br / 2);
    g_word_count = word_count;

    /* Pre-scan for BUTTON_DEF offset and the initial variable block. */
    for (uint16_t i = 0; i < word_count; i++)
        if (get_word(i) == 0xeaee) button_def_pc = i + 2;

    uint16_t pc = 0;
    if (word_count > 0 && get_word(0) == 0xe8e8) {
        uint16_t vi = 1; pc = 1;
        while (pc < word_count && get_word(pc) != 0xe8e8) {
            if (vi < 1024) variables[vi++] = get_word(pc);
            pc++;
        }
        if (pc < word_count) pc++;
    }

    /* Pre-apply initial ATTACKMODE so USB enumerates in the right mode. */
    if (pc < word_count) {
        uint16_t w = get_word(pc);
        if (w==0xf0f0 || w==0xf1f1 || w==0xf2f2 || w==0xf3f3) {
            uint16_t tmp = pc;
            parse_attackmode(&tmp, word_count);
        }
    }

    /* Pre-cache the SD sectors we'll need while USB owns the SPI bus. */
    mc_precache(0);
    mc_precache(fs.fatbase);
    if (fs.fs_type == 3) mc_precache(fs.database);
    else                 mc_precache(fs.dirbase);

    start_usb_and_wait();

    /* Wait for the host to start polling the HID IN endpoint, then scrub any
     * latent modifier state.  After this point, no Shift/Ctrl/Alt/GUI can be
     * "held" on the host side.  Only meaningful when HID is active (mode 1 or
     * the composite mode 3). */
    if (current_attackmode == 1 || current_attackmode == 3) {
        wait_for_host_polling();
        hid_scrub(3);
    }

    /* ---- Wait for the SD mount to finish BEFORE the first keystroke -------
     * This lives in the firmware (not the payload) so it works no matter how
     * the DuckyScript is written — including a payload whose very first line is
     * a STRING with no leading DELAY.  Without it, that first STRING is typed
     * straight into Windows's mount storm and all but its tail is lost.
     *
     * Storage runs at FULL speed here.  We (a) wait for the mount to actually
     * begin (first storage command), then (b) return as soon as the card has
     * been quiet for a short spell = mount complete.  So the wait is just the
     * card's real mount time, not a fixed stall; the caps guard a weird host. */
    /* No fixed startup mount-wait.  Typing begins immediately; send_key()
     * self-tunes — it spaces keystrokes out and lets storage run full-speed
     * WHILE the mount is busy (so the first STRING is clean even though it
     * overlaps the mount, and the mount finishes fast), then switches to fast
     * typing + a storage trickle the moment the card goes quiet.  Seed
     * g_last_msc_cyc to "now" so the mount-done latch starts un-latched. */
    g_last_msc_cyc = cyc();
    usb_msc_set_budget(0);
    hid_warmup();

    g_payload_run = true;
    blink_t0 = cyc();

    /* =================================================================
     * Interpreter
     * ================================================================= */
    while (pc < word_count) {
        usb_device_task();
        blink_tick();

        /* Implicit button → BUTTON_DEF jump (only if the payload has a
         * BUTTON_DEF block AND we're not already inside one). */
        if (!gpio_read(BTN_PORT, BTN_PIN)
            && button_enabled && !in_button_handler && button_def_pc != 0)
        {
            button_push_received = 1;
            if (call_stack_ptr < 32) {
                in_button_handler       = true;
                call_stack[call_stack_ptr++] = pc;
                pc = button_def_pc;
                continue;
            }
        }

        uint16_t word = get_word(pc);

        /* -------- Control opcodes (caught before raw key default) ----- */
        if (word == 0xe801) {                 /* ASSIGNMENT */
            uint16_t dest = get_word(pc+1);
            uint16_t s1   = get_word(pc+2);
            uint16_t op   = get_word(pc+3);
            if (op == 0) { write_var(dest, read_var(s1)); pc += 4; }
            else {
                uint16_t s2 = get_word(pc+4);
                write_var(dest, eval_op(op, read_var(s1), read_var(s2)));
                pc += 5;
            }
            continue;
        }
        if (word == 0xefef) {                 /* IF */
            uint16_t cond = get_word(pc+1);
            uint16_t bid  = get_word(pc+2);
            if (!read_var(cond)) {
                uint16_t s = pc + 3;
                while (s < word_count) {
                    if (get_word(s) == 0x1ff4 && get_word(s+1) == bid) { pc = s + 2; break; }
                    s++;
                }
                if (s >= word_count) pc += 3;
            } else pc += 3;
            continue;
        }
        if (word == 0x1ff4) { pc += 2; continue; }     /* END_IF */
        if (word == 0xf8f8) {                          /* GOTO */
            uint16_t t = get_word(pc+1);
            pc = (uint16_t)((t>>8) | ((t & 0xFF) << 8));
            continue;
        }
        if (word == 0xf7f7) {                          /* CALL */
            if (call_stack_ptr < 32) {
                call_stack[call_stack_ptr++] = pc + 2;
                uint16_t t = get_word(pc+1);
                pc = (uint16_t)((t>>8) | ((t & 0xFF) << 8));
            } else pc += 2;
            continue;
        }
        if (word == 0xfdfd) {                          /* RETURN */
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc++;
            continue;
        }
        if (word == 0xfff8) {                          /* KEY_DOWN (intentional hold) */
            uint16_t k = get_word(pc+1);
            g_allow_bare_modifier = true;   /* allow holding a bare modifier */
            hid_send_report((uint8_t)(k & 0xFF), (uint8_t)(k >> 8));
            g_allow_bare_modifier = false;
            pc += 2; continue;
        }
        if (word == 0xeee8) {                          /* KEY_UP */
            hid_release_all();
            pc += 2; continue;
        }
        if (word == 0xeaee) {                          /* BUTTON_DEF — skip body */
            uint16_t bid = get_word(pc+1);
            uint16_t s   = pc + 2;
            while (s < word_count) {
                if (get_word(s) == 0xebf4 && get_word(s+1) == bid) { pc = s + 2; break; }
                s++;
            }
            if (s >= word_count) pc += 2;
            continue;
        }
        if (word == 0xebf4) {                          /* END_BUTTON */
            in_button_handler = false;
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc += 2;
            continue;
        }
        if (word == 0xe9e9) {                          /* INJECT_VAR / RANDOM_* */
            uint16_t arg = get_word(pc+1);
            if      (arg==0xf442) type_char((char)rand_range('a','z'));
            else if (arg==0xf542) type_char((char)rand_range('A','Z'));
            else if (arg==0xf642) type_char(rand_range(0,1) ? (char)rand_range('a','z') : (char)rand_range('A','Z'));
            else if (arg==0xf742) type_char((char)rand_range('0','9'));
            else if (arg==0xf842) {
                const char sp[] = "!@#$%^&*()_+-=[]{}|;':\",./<>?~`";
                type_char(sp[rand_range(0, (uint8_t)(sizeof(sp)-2))]);
            } else if (arg==0xf942) type_char((char)rand_range(32, 126));
            else {
                uint16_t v = read_var(arg);
                if (v >= 32 && v <= 126) type_char((char)v);
                else                     type_number((int)v);
            }
            pc += 2; continue;
        }
        if (word == 0xe7e9) {                          /* DELAY_VAR */
            payload_delay(read_var(get_word(pc+1)));
            pc += 2; continue;
        }
        if (word == 0xf6e9) { pc += 2; continue; }     /* EXFIL_VAR stub */
        if (word==0xf0f0 || word==0xf1f1 || word==0xf2f2 || word==0xf3f3) {
            parse_attackmode(&pc, word_count);
            apply_attackmode();
            continue;
        }
        if ((word >> 8) == 0x00) {                     /* DELAY literal */
            payload_delay(word & 0xFF);
            pc++; continue;
        }

        /* -------- Builtins / raw keystroke (default) ------------------ */
        switch (word) {
            case 0x04ed:                                 break;       /* RESET (no-op) */
            case 0xebee: button_enabled = 0;             break;
            case 0xecee: button_enabled = 1;             break;
            case 0xebf1:                                              /* STOP_PAYLOAD */
                hid_release_all();
                led_green();
                usb_msc_set_suppressed(false); /* idle: allow SD to mount */
                while (1) usb_device_task();
                break;
            case 0xeaed: led_off();                      break;
            case 0xebed: led_green();                    break;
            case 0xeced: led_green();                    break;
            case 0xeeed: led_red();                      break;
            case 0xeffe: led_red();                      break;
            case 0xedee: system_leds_enabled = 1;        break;
            case 0xeeee: system_leds_enabled = 0;        break;
            case 0xeaeb:                                              /* SAVE_HOST_LOCK_STATE */
                saved_caps_lock_on  = caps_lock_on;
                saved_num_lock_on   = num_lock_on;
                saved_scroll_on     = scroll_lock_on;
                break;
            case 0xebeb:                                              /* RESTORE_HOST_LOCK_STATE */
                if (caps_lock_on   != saved_caps_lock_on)  send_key(0, 0x39);
                if (num_lock_on    != saved_num_lock_on)   send_key(0, 0x53);
                if (scroll_lock_on != saved_scroll_on)     send_key(0, 0x47);
                break;
            case 0xeae9:                                              /* SAVE_ATTACKMODE */
                saved_attackmode = current_attackmode;
                saved_vid        = current_vid;
                saved_pid        = current_pid;
                break;
            case 0xebe9:                                              /* RESTORE_ATTACKMODE */
                current_attackmode = (uint8_t)saved_attackmode;
                current_vid        = saved_vid;
                current_pid        = saved_pid;
                apply_attackmode();
                break;
            case 0x01ea: while (!caps_lock_on)   { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x02ea: while ( caps_lock_on)   { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x03ea: { uint16_t s=caps_lock_on;   while (caps_lock_on==s)   { usb_keepalive(); blink_tick(); poll_button(); } break; }
            case 0x04ea: while (!num_lock_on)    { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x05ea: while ( num_lock_on)    { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x06ea: { uint16_t s=num_lock_on;    while (num_lock_on==s)    { usb_keepalive(); blink_tick(); poll_button(); } break; }
            case 0x07ea: while (!scroll_lock_on) { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x08ea: while ( scroll_lock_on) { usb_keepalive(); blink_tick(); poll_button(); } break;
            case 0x09ea: { uint16_t s=scroll_lock_on; while (scroll_lock_on==s) { usb_keepalive(); blink_tick(); poll_button(); } break; }

            case 0xeaea: {                                            /* WAIT_FOR_BUTTON_PRESS */
                /* Scrub host-side modifier state BEFORE the wait. */
                hid_scrub(3);
                button_push_received = 0;

                /* Idle window: unlimited storage so the drive stays alive /
                 * finishes mounting while the user decides to press. */
                usb_msc_set_budget(-1);

                /* Consume a latched press; otherwise wait for a fresh one. */
                if (button_pending) {
                    button_pending = 0;
                } else {
                    while (gpio_read(BTN_PORT, BTN_PIN)) {
                        usb_keepalive();
                        blink_tick();
                        if (button_pending) { button_pending = 0; break; }
                    }
                }
                button_push_received = 1;

                /* Wait for release (with debounce), keeping HID warm. */
                delay_ms(20);
                while (!gpio_read(BTN_PORT, BTN_PIN)) usb_keepalive();
                delay_ms(20);
                button_pending = 0;

                /* Quiet storage again, then warm up HID polling (also scrubs
                 * modifiers) so the STRING after the button doesn't drop its
                 * leading characters.  send_key() trickles storage from here. */
                usb_msc_set_budget(0);
                hid_warmup();
                break;
            }

            default: {                                                /* Raw HID key (keycode<<8 | mod) */
                uint8_t keycode  = (uint8_t)(word >> 8);
                uint8_t modifier = (uint8_t)(word & 0xFF);
                send_key(modifier, keycode);
                break;
            }
        }
        pc++;
    }

    g_payload_run = false;
    led_green();
    usb_msc_set_suppressed(false); /* idle: let the SD card stay/finish mounting */
    while (1) usb_device_task();
    return 0;
}
