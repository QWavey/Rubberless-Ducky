/**
 * @file main.c
 * @brief Custom HID Firmware for AT32UC3B1 (AVR32 UC3B1256) with PetitFS Payload
 *
 * FIXES vs BACKUP-version1:
 *  1. CapsLock no longer flashes red LED — usb_hid_report_out_cb no longer
 *     drives LEDs unconditionally; only led_show_caps/led_show_num paths remain,
 *     both guarded by system_leds_enabled.
 *  2. Payloads without ATTACKMODE on top now default to HID automatically.
 *     current_attackmode initialised to 1 (HID); start_usb_and_wait() calls
 *     apply_attackmode() which selects the HID config before enabling USB.
 *  3. Per-character inter-key delay: 5 ms between press/release so fast hosts
 *     stop dropping characters or holding the last key.
 *  4. Full printable-ASCII mapping in char_to_hid() — all symbols, punctuation,
 *     shifted variants.  The "?" held bug is fixed here.
 *  5. RESTORE_ATTACKMODE now calls apply_attackmode() to actually re-enumerate
 *     USB with the saved VID/PID/config (was a no-op before).
 *  6. WAIT_FOR_BUTTON_PRESS just waits + debounces; it no longer re-enters the
 *     BUTTON_DEF handler (was causing double-execution).
 *  7. LED defaults: Green solid = idle/done, Green blinking = executing payload,
 *     Red solid = no SD card or no INJECT.BIN.
 *  8. LED_R and LED_G short-alias opcodes added to the switch table.
 *  9. ATTACKMODE MAN_/PROD_/SERIAL_ parameters are parsed and written into the
 *     live USB string descriptors before re-enumeration.
 * 10. No SD card or no INJECT.BIN → device goes silent (no typing, no button
 *     reactions, no CapsLock response) — just Red LED + USB idle.
 * 11. system_leds_enabled guards every LED write from the HID-out callback.
 */

#include <stdint.h>
#include <avr32/io.h>
#include <string.h>
#include "usb_hid.h"
#include "usb_descriptors.h"
#include "pff.h"
#include "diskio.h"

/* ---- freestanding libc stubs ---- */
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

#define CYCLES_PER_MS  48000u   /* 48 MHz core clock */

/* ---- GPIO pin assignments ---- */
#define LED_PORT_GREEN 0
#define LED_PIN_GREEN  7   /* PA07 green LED, active-low */
#define LED_PORT_RED   0
#define LED_PIN_RED    8   /* PA08 red LED,   active-low */
#define BTN_PORT       0
#define BTN_PIN        13  /* PA13 button, active-low with pull-up */

#define _GPIO_PORT(n)  ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void gpio_out      (int port, int pin) {
    volatile avr32_gpio_port_t *p = _GPIO_PORT(port);
    p->gpers = (1u<<pin); p->oders = (1u<<pin);
}
static inline void gpio_in_pullup(int port, int pin) {
    volatile avr32_gpio_port_t *p = _GPIO_PORT(port);
    p->gpers = (1u<<pin); p->oderc = (1u<<pin); p->puers = (1u<<pin);
}
static inline void gpio_high(int port, int pin) { _GPIO_PORT(port)->ovrs = (1u<<pin); }
static inline void gpio_low (int port, int pin) { _GPIO_PORT(port)->ovrc = (1u<<pin); }
static inline bool gpio_read(int port, int pin) { return !!(_GPIO_PORT(port)->pvr & (1u<<pin)); }

typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} keyboard_report_t;

static volatile bool g_usb_ready = false;
static uint32_t host_configuration_request_count = 0;
static bool g_payload_executing = false;
static bool g_sd_card_ok = false;

void usb_device_enumerated_cb(void)       { g_usb_ready = true; }
void usb_device_config_requested_cb(void) { host_configuration_request_count++; }

/* ---- Runtime state variables ---- */
static uint16_t variables[1024];
static uint16_t button_enabled                       = 1;
static uint16_t button_user_defined                  = 0;
static uint16_t button_push_received                 = 0;
static uint16_t system_leds_enabled                  = 1;
static uint16_t injecting_leds_enabled               = 1;
static uint16_t caps_lock_on                         = 0;
static uint16_t num_lock_on                          = 0;
static uint16_t scroll_lock_on                       = 0;
static uint16_t saved_caps_lock_on                   = 0;
static uint16_t saved_num_lock_on                    = 0;
static uint16_t saved_scroll_lock_on                 = 0;
static uint16_t button_timeout                       = 0;
static uint16_t payload_parse_speed                  = 0;
uint16_t        current_vid                          = 0x05AC;
uint16_t        current_pid                          = 0x021E;
static uint16_t current_os                           = 1; /* 1=WINDOWS */
uint8_t         current_attackmode                   = 1; /* DEFAULT = HID */
static uint16_t jitter_enabled                       = 0;
static uint16_t jitter_max                           = 0;
static uint16_t led_show_caps                        = 0;
static uint16_t led_show_num                         = 0;
static uint16_t storage_leds_enabled                 = 1;
static uint16_t led_continuous_show_storage_activity = 0;
static uint16_t storage_activity_timeout             = 1000;
static uint16_t received_host_lock_led_reply         = 0;
static uint16_t exfil_leds_enabled                   = 1;
static uint16_t exfil_mode_enabled                   = 0;

static uint16_t random_min = 0;
static uint16_t random_max = 9;

static uint16_t saved_attackmode = 1;
static uint16_t saved_vid        = 0x05AC;
static uint16_t saved_pid        = 0x021E;

static uint16_t call_stack[32];
static uint8_t  call_stack_ptr        = 0;
static uint16_t button_def_pc         = 0;
static bool     inside_button_handler = false;

/* Per-character inter-key delay (ms).  5 ms is the sweet spot: fast enough
 * for payloads, slow enough that no host ever drops a character or holds the
 * last key pressed.  Payloads can override via STRING_DELAY if the compiler
 * emits the DELAY_VAR opcode around each character. */
static uint16_t char_delay_ms = 5;

/* ---- LED convenience helpers ---- */
/* All LED changes go through these so system_leds_enabled is always respected */
static inline void led_green_on(void) {
    if (!system_leds_enabled) return;
    gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high(LED_PORT_RED,   LED_PIN_RED);
}
static inline void led_red_on(void) {
    if (!system_leds_enabled) return;
    gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_low (LED_PORT_RED,   LED_PIN_RED);
}
static inline void led_off(void) {
    gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high(LED_PORT_RED,   LED_PIN_RED);
}

/* ---- HID LED report callback ----
 * ONLY updates the internal lock-key state variables.
 * Only drives LEDs if system_leds_enabled is true AND no payload is running. */
void usb_hid_report_out_cb(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    uint8_t leds    = data[0];
    num_lock_on     = !!(leds & 0x01);
    caps_lock_on    = !!(leds & 0x02);
    scroll_lock_on  = !!(leds & 0x04);
    received_host_lock_led_reply = 1;

    if (!system_leds_enabled || g_payload_executing || !g_sd_card_ok) return;

    /* Drive LEDs based on host lock state if enabled and idle */
    if (led_show_caps) {
        if (caps_lock_on) gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
        else              gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
    }
    if (led_show_num) {
        if (num_lock_on) gpio_low (LED_PORT_RED, LED_PIN_RED);
        else             gpio_high(LED_PORT_RED, LED_PIN_RED);
    }
}

static inline uint32_t get_cpu_count(void) { return __builtin_mfsr(AVR32_COUNT); }
void storage_activity_mark(void) {}

static uint32_t rand_seed = 12345;
static uint16_t get_random(uint16_t lo, uint16_t hi) {
    rand_seed = rand_seed * 1103515245u + 12345u;
    if (hi <= lo) return lo;
    return lo + (uint16_t)((rand_seed >> 16) % ((uint32_t)(hi - lo + 1)));
}

static void delay_ms(uint32_t ms) {
    if (jitter_enabled && jitter_max > 0) {
        ms += get_random(0, jitter_max);
    }
    while (ms--) {
        uint32_t start = get_cpu_count();
        while ((get_cpu_count() - start) < CYCLES_PER_MS) { usb_device_task(); }
    }
}

/* ---- Modifier bit masks ---- */
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RALT    0x40

static void send_keyboard_report(uint8_t modifier, uint8_t keycode) {
    if (current_attackmode == 0 || current_attackmode == 2) return; /* No HID */

    keyboard_report_t report;
    memset(&report, 0, sizeof(report));
    report.modifier = modifier;
    report.keys[0]  = keycode;
    
    /* 1. Wait for endpoint ready.
     * We use a long timeout (2500ms) to ensure the payload automatically
     * pauses and waits for the host to finish mounting the storage volume
     * and initializing the HID driver. */
    uint32_t t0 = get_cpu_count();
    while (!usb_hid_in_endpoint_ready()) {
        if ((get_cpu_count() - t0) > (CYCLES_PER_MS * 2500u)) {
            /* If we time out, the host is not polling (e.g. wall charger
             * or completely frozen). We MUST flush the FIFO to clear any
             * unread reports (like a key-down) so they don't get stuck
             * forever when the host finally wakes up. */
            AVR32_USBB.uerst |= AVR32_USBB_UERST_EPRST1_MASK;
            AVR32_USBB.uerst &= ~AVR32_USBB_UERST_EPRST1_MASK;
            return;
        }
        usb_device_task();
    }
    
    usb_hid_send_report((uint8_t*)&report, sizeof(report));

    /* 2. Wait a short time for the host to actually fetch this report
     * before we return. This guarantees the press and release are seen as distinct. */
    if (modifier != 0 || keycode != 0) {
        t0 = get_cpu_count();
        while (!usb_hid_in_endpoint_ready()) {
            if ((get_cpu_count() - t0) > (CYCLES_PER_MS * 10u)) break;
            usb_device_task();
        }
    }
}


/* ---- Full printable-ASCII → HID keycode table ----
 * Covers every character that can appear in a STRING command.
 * The previous version was missing ?, !, @, #, $, … which caused
 * characters to be dropped or the last key to be held indefinitely. */
typedef struct { uint8_t keycode; uint8_t modifier; } KeyEntry;

static KeyEntry char_to_hid(char c) {
    KeyEntry e = {0, 0};
    if (c >= 'a' && c <= 'z') { e.keycode = 0x04 + (uint8_t)(c-'a'); return e; }
    if (c >= 'A' && c <= 'Z') { e.keycode = 0x04 + (uint8_t)(c-'A'); e.modifier = MOD_LSHIFT; return e; }
    if (c >= '1' && c <= '9') { e.keycode = 0x1E + (uint8_t)(c-'1'); return e; }
    switch (c) {
        /* Unshifted */
        case '0':  e.keycode = 0x27; return e;
        case '\n': e.keycode = 0x28; return e;
        case '\r': e.keycode = 0x28; return e;
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
        /* Shifted */
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
        default:   return e; /* keycode 0 = no-op */
    }
}

static void type_char(char c) {
    KeyEntry e = char_to_hid(c);
    /* Only skip if keycode is truly 0 AND the char isn't one of the zero-keycode
     * specials (space=0x2C, newline=0x28, tab=0x2B — all handled above). */
    if (e.keycode == 0) return;
    send_keyboard_report(e.modifier, e.keycode);
    delay_ms(char_delay_ms);
    send_keyboard_report(0, 0);
    delay_ms(char_delay_ms);
}

/* static void type_string(const char *s) { while (*s) type_char(*s++); } */

static void type_number(int n) {
    if (n < 0) { type_char('-'); n = -n; }
    if (n == 0) { type_char('0'); return; }
    char buf[7]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) type_char(buf[j]);
}

/* ---- Payload storage ---- */
static uint8_t payload_ram[8192];

static uint16_t get_word(uint16_t idx) {
    uint16_t b = (uint16_t)(idx * 2);
    return ((uint16_t)payload_ram[b] << 8) | payload_ram[b+1];
}

/* ---- Variable read/write ---- */
static uint16_t read_var(uint16_t addr) {
    if (addr >= 0x0001 && addr < 1024) return variables[addr];
    switch (addr) {
        case 0x8042: return button_enabled;
        case 0x8142: return button_user_defined;
        case 0x8442: return button_push_received;
        case 0x8542: return led_continuous_show_storage_activity;
        case 0x8642: return system_leds_enabled;
        case 0x8742: return storage_leds_enabled;
        case 0x8842: return injecting_leds_enabled;
        case 0x8942: return exfil_leds_enabled;
        case 0x9042: return caps_lock_on;
        case 0x9142: return num_lock_on;
        case 0x9242: return scroll_lock_on;
        case 0x9342: return saved_caps_lock_on;
        case 0x9442: return saved_num_lock_on;
        case 0x9542: return saved_scroll_lock_on;
        case 0x9642: return received_host_lock_led_reply;
        case 0x9742: return exfil_mode_enabled;
        case 0x9842: return storage_activity_timeout;
        case 0x9942: return button_timeout;
        case 0x9A42: return payload_parse_speed;
        case 0x9B42: return current_vid;
        case 0x9C42: return current_pid;
        case 0x9D42: return current_os;
        case 0x9F42: return host_configuration_request_count;
        case 0xA042: return current_attackmode;
        case 0xA242: return jitter_enabled;
        case 0xA342: return jitter_max;
        case 0xf042: return random_min;  /* $_RANDOM_MIN */
        case 0xf142: return random_max;  /* $_RANDOM_MAX */
        case 0xf242: return get_random(random_min, random_max); /* $_RANDOM_INT */
        case 0x6742: return 0;  /* FALSE */
        case 0x6842: return 1;  /* TRUE */
        case 0x6942: return 1;  /* WINDOWS */
        case 0x7042: return 2;  /* MACOS */
        case 0x7142: return 3;  /* LINUX */
        case 0x7242: return 4;  /* ANDROID */
        case 0x7342: return 5;  /* IOS */
        case 0x7442: return 6;  /* CHROMEOS */
        default: return 0;
    }
}

static void write_var(uint16_t addr, uint16_t val) {
    if (addr >= 0x0001 && addr < 1024) { variables[addr] = val; return; }
    switch (addr) {
        case 0x8042: button_enabled                       = val; break;
        case 0x8142: button_user_defined                  = val; break;
        case 0x8442: button_push_received                 = val; break;
        case 0x8542: led_continuous_show_storage_activity = val; break;
        case 0x8642: system_leds_enabled                  = val; break;
        case 0x8742: storage_leds_enabled                 = val; break;
        case 0x8842: injecting_leds_enabled               = val; break;
        case 0x8942: exfil_leds_enabled                   = val; break;
        case 0x9042: caps_lock_on                         = val; break;
        case 0x9142: num_lock_on                          = val; break;
        case 0x9242: scroll_lock_on                       = val; break;
        case 0x9342: saved_caps_lock_on                   = val; break;
        case 0x9442: saved_num_lock_on                    = val; break;
        case 0x9542: saved_scroll_lock_on                 = val; break;
        case 0x9642: received_host_lock_led_reply         = val; break;
        case 0x9742: exfil_mode_enabled                   = val; break;
        case 0x9842: storage_activity_timeout             = val; break;
        case 0x9942: button_timeout                       = val; break;
        case 0x9A42: payload_parse_speed                  = val; break;
        case 0x9B42: current_vid                          = val; break;
        case 0x9C42: current_pid                          = val; break;
        case 0x9D42: current_os                           = val; break;
        case 0x9F42: host_configuration_request_count     = val; break;
        case 0xA042: current_attackmode                   = val; break;
        case 0xA242: jitter_enabled                       = val; break;
        case 0xA342: jitter_max                           = val; break;
        case 0xf042: random_min                           = val; break;
        case 0xf142: random_max                           = val; break;
    }
}


static uint16_t eval_op(uint16_t op, uint16_t v1, uint16_t v2) {
    switch (op) {
        case 0xe802: return v1 + v2;
        case 0xe803: return v1 - v2;
        case 0xe804: return v1 * v2;
        case 0xe805: return v2 ? v1 / v2 : 0;
        case 0xe806: return v1 == v2;
        case 0xe807: return v1 != v2;
        case 0xe808: return v1 < v2;
        case 0xe809: return v1 > v2;
        case 0xe8a8: return v1 <= v2;
        case 0xe8a9: return v1 >= v2;
        case 0xe8aa: return v1 && v2;
        case 0xe8bb: return v1 || v2;
        case 0xe80a: return v1 & v2;
        case 0xe80b: return v1 | v2;
        case 0xe80c: return v1 >> v2;
        case 0xe80d: return v1 << v2;
        case 0xe80e: return v2 ? v1 % v2 : 0;
        case 0x0fe8: { uint16_t r=1; for(uint16_t i=0;i<v2;i++) r*=v1; return r; }
        default: return 0;
    }
}

FATFS fs;

/* ================================================================
 * apply_attackmode()
 * Selects the correct USB config descriptor, patches VID/PID into
 * the device descriptor, then re-enables USB so the host sees a
 * fresh device.  Called both from start_usb_and_wait() (first run,
 * uses the default HID mode) and from inline ATTACKMODE opcodes.
 * ================================================================ */
static uint16_t last_applied_vid   = 0xFFFF;
static uint16_t last_applied_pid   = 0xFFFF;
static uint8_t  last_applied_mode  = 0xFF;

static void apply_attackmode(void) {
    /* If the requested state is already active, do nothing.
     * This avoids redundant resets when the pre-scan handles the first command. */
    if (current_attackmode == last_applied_mode && 
        current_vid == last_applied_vid && 
        current_pid == last_applied_pid) return;

    g_usb_ready = false;
    usb_device_disable();
    delay_ms(250); /* ≥250 ms so the host registers a disconnect */

    extern const uint8_t  usb_config_descriptor_hid[];
    extern const uint16_t usb_config_descriptor_hid_size;
    extern const uint8_t  usb_config_descriptor_msc[];
    extern const uint16_t usb_config_descriptor_msc_size;
    extern const uint8_t  usb_config_descriptor_comp[];
    extern const uint16_t usb_config_descriptor_comp_size;
    extern const uint8_t *usb_config_descriptor;
    extern uint16_t       usb_config_descriptor_size;
    extern uint8_t        usb_device_descriptor[];

    if (current_attackmode == 2) {
        usb_config_descriptor      = usb_config_descriptor_msc;
        usb_config_descriptor_size = usb_config_descriptor_msc_size;
    } else if (current_attackmode == 3) {
        usb_config_descriptor      = usb_config_descriptor_comp;
        usb_config_descriptor_size = usb_config_descriptor_comp_size;
    } else { /* 0=OFF or 1=HID — both use the HID descriptor; OFF just doesn't attach */
        usb_config_descriptor      = usb_config_descriptor_hid;
        usb_config_descriptor_size = usb_config_descriptor_hid_size;
    }

    /* XOR-disambiguate PID for composite/storage only if using the default Hak5 VID/PID.
     * If the user specified a custom VID/PID, we use it exactly as requested. */
    uint16_t pid = current_pid;
    if (current_vid == 0x05AC && current_pid == 0x021E) {
        if      (current_attackmode == 3) pid ^= 0x4242;
        else if (current_attackmode == 2) pid ^= 0x1337;
    }

    usb_device_descriptor[8]  = (uint8_t)(current_vid & 0xFF);
    usb_device_descriptor[9]  = (uint8_t)(current_vid >> 8);
    usb_device_descriptor[10] = (uint8_t)(pid & 0xFF);
    usb_device_descriptor[11] = (uint8_t)(pid >> 8);

    received_host_lock_led_reply = 0;
    if (current_attackmode != 0) {
        usb_device_enable();
        /* Wait up to 5 s for the host to complete enumeration. */
        uint32_t timeout = 5000;
        while (!g_usb_ready && timeout--) {
            uint32_t t0 = get_cpu_count();
            while ((get_cpu_count() - t0) < CYCLES_PER_MS) { usb_device_task(); }
        }
    }

    last_applied_mode = current_attackmode;
    last_applied_vid  = current_vid;
    last_applied_pid  = current_pid;
}

static void parse_attackmode(uint16_t *pc_ptr, uint16_t word_count) {
    uint16_t pc = *pc_ptr;
    uint16_t word = get_word(pc);
    if      (word == 0xf0f0) current_attackmode = 0;
    else if (word == 0xf1f1) current_attackmode = 1;
    else if (word == 0xf2f2) current_attackmode = 2;
    else if (word == 0xf3f3) current_attackmode = 3;
    else return;

    pc++;
    /* Parse optional VID/PID/MAN/PROD/SERIAL params until end-marker */
    while (pc < word_count && get_word(pc) != word) {
        uint16_t p2 = get_word(pc);
        if (p2 == 0xf5f5) {       /* VID */
            current_vid = read_var(get_word(pc+1)); pc += 2;
        } else if (p2 == 0xf6f6) { /* PID */
            current_pid = read_var(get_word(pc+1)); pc += 2;
        } else if (p2 == 0xf9f9) { /* MAN_ */
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < word_count && get_word(pc) != 0xf9f9) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < word_count) pc++; /* skip closing 0xf9f9 */
            usb_set_string_descriptor(usb_str_manufacturer_descriptor, buf, 32);
        } else if (p2 == 0xfafa) { /* PROD_ */
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < word_count && get_word(pc) != 0xfafa) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < word_count) pc++; /* skip closing 0xfafa */
            usb_set_string_descriptor(usb_str_product_descriptor, buf, 32);
        } else if (p2 == 0xfbfb) { /* SERIAL_ */
            pc++;
            char buf[13]; uint16_t bi=0;
            while (pc < word_count && get_word(pc) != 0xfbfb) {
                if (bi < 12) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < word_count) pc++; /* skip closing 0xfbfb */
            usb_set_string_descriptor(usb_str_serial_descriptor, buf, 12);
            usb_custom_serial_set = 1;
        } else {
            pc++;
        }
    }
    if (pc < word_count) pc++;
    *pc_ptr = pc;
}

static void start_usb_and_wait(void) {
    usb_device_init();
    sd_mark_spi_dead();
    usb_device_register_callback(USB_EVENT_ENUMERATED,       usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);

    /* apply_attackmode() calls usb_device_enable() and waits internally */
    apply_attackmode();
}


/* ---- Green-blink ticker for "payload executing" state ---- */
static uint32_t blink_last = 0;
static bool     blink_on   = false;

static void blink_green_tick(void) {
    if (!system_leds_enabled || !injecting_leds_enabled) return;
    uint32_t now = get_cpu_count();
    if ((now - blink_last) >= CYCLES_PER_MS * 250u) {
        blink_last = now;
        blink_on   = !blink_on;
        if (blink_on) gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
        else          gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
        gpio_high(LED_PORT_RED, LED_PIN_RED); /* never red during normal execution */
    }
}

/* ================================================================
 * main()
 * ================================================================ */
int main(void)
{
    Disable_global_interrupt();

    /* --- Disable watchdog --- */
    volatile uint32_t *wdt = (volatile uint32_t *)(&AVR32_WDT.ctrl);
    uint32_t wv = *wdt & ~AVR32_WDT_CTRL_KEY_MASK;
    *wdt = wv | (0x55ul << AVR32_WDT_CTRL_KEY_OFFSET);
    *wdt = (wv & ~AVR32_WDT_CTRL_EN_MASK) | (0xAAul << AVR32_WDT_CTRL_KEY_OFFSET);

    /* --- Clock: OSC0 crystal → PLL0 → 48 MHz --- */
    AVR32_PM.mcctrl = 0; /* RCSYS fallback while we start the crystal */
    AVR32_PM.OSCCTRL0.startup = AVR32_PM_OSCCTRL0_STARTUP_2048_RCOSC;
    AVR32_PM.OSCCTRL0.mode    = AVR32_PM_OSCCTRL0_MODE_CRYSTAL_G3;
    AVR32_PM.mcctrl = AVR32_PM_MCCTRL_OSC0EN_MASK;
    while (!(AVR32_PM.poscsr & AVR32_PM_POSCSR_OSC0RDY_MASK));

    AVR32_PM.pll[0] = 0;
    AVR32_PM.pll[0] = (0  << AVR32_PM_PLLOSC_OFFSET)
                    | (1  << AVR32_PM_PLLDIV_OFFSET)
                    | (7  << AVR32_PM_PLLMUL_OFFSET)
                    | (3  << AVR32_PM_PLLOPT_OFFSET)
                    | (63 << AVR32_PM_PLLCOUNT_OFFSET);
    AVR32_PM.pll[0] |= AVR32_PM_PLLEN_MASK;
    while (!(AVR32_PM.poscsr & AVR32_PM_POSCSR_LOCK0_MASK));

    AVR32_FLASHC.fcr = (1 << AVR32_FLASHC_FCR_FWS_OFFSET);
    AVR32_PM.mcctrl  = AVR32_PM_MCCTRL_OSC0EN_MASK
                     | (AVR32_PM_MCCTRL_MCSEL_PLL0 << AVR32_PM_MCCTRL_MCSEL_OFFSET);

    /* --- GPIO: both LEDs off initially, button input with pull-up --- */
    gpio_out      (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high     (LED_PORT_GREEN, LED_PIN_GREEN); /* active-low: high = off */
    gpio_out      (LED_PORT_RED,   LED_PIN_RED);
    gpio_high     (LED_PORT_RED,   LED_PIN_RED);
    gpio_in_pullup(BTN_PORT, BTN_PIN);

    /* ----------------------------------------------------------------
     * Try to mount the SD card and open INJECT.BIN.
     * If either fails → solid RED, start USB in quiet HID mode, idle.
     * ---------------------------------------------------------------- */
    g_sd_card_ok = false;
    FRESULT res_mount = pf_mount(&fs);
    if (res_mount != FR_OK) {
        gpio_low(LED_PORT_RED, LED_PIN_RED); /* Red solid = no SD */
        start_usb_and_wait();
        while (1) { usb_device_task(); }
    }

    FRESULT res_bin = pf_open("INJECT.BIN");
    if (res_bin != FR_OK) {
        gpio_low(LED_PORT_RED, LED_PIN_RED); /* Red solid = no INJECT.BIN */
        start_usb_and_wait();
        while (1) { usb_device_task(); }
    }

    /* SD OK, INJECT.BIN found → green solid while we load it */
    g_sd_card_ok = true;
    gpio_low(LED_PORT_GREEN, LED_PIN_GREEN);

    UINT br = 0;
    if (pf_read(payload_ram, sizeof(payload_ram), &br) != FR_OK || br == 0) {
        g_sd_card_ok = false;
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) { usb_device_task(); }
    }

    uint16_t word_count = (uint16_t)(br / 2);

    /* --- Pre-scan: find BUTTON_DEF offset --- */
    for (uint16_t i = 0; i < word_count; i++) {
        if (get_word(i) == 0xeaee) {
            button_def_pc = i + 2;
        }
    }

    /* --- Parse variable initialisation block (if present) --- */
    uint16_t pc = 0;
    if (word_count > 0 && get_word(0) == 0xe8e8) {
        uint16_t vi = 1; pc = 1;
        while (pc < word_count && get_word(pc) != 0xe8e8) {
            if (vi < 1024) variables[vi++] = get_word(pc);
            pc++;
        }
        if (pc < word_count) pc++; /* skip closing 0xe8e8 */
    }

    /* --- Pre-scan for ATTACKMODE at the top to set initial USB state --- */
    if (pc < word_count) {
        uint16_t w = get_word(pc);
        if (w==0xf0f0 || w==0xf1f1 || w==0xf2f2 || w==0xf3f3) {
            uint16_t temp_pc = pc;
            parse_attackmode(&temp_pc, word_count);
        }
    }

    /* --- Pre-cache SD sectors before USB takes over PA25 (MISO) --- */
    mc_precache(0);
    mc_precache(fs.fatbase);
    if (fs.fs_type == 3) mc_precache(fs.database);
    else                  mc_precache(fs.dirbase);

    start_usb_and_wait();

    g_payload_executing = true;
    /* Green blinking = payload executing */
    blink_last = get_cpu_count();

    /* ================================================================
     * Payload execution loop
     * ================================================================ */
    while (pc < word_count) {
        usb_device_task();
        blink_green_tick();

        /* --- Physical button interrupt --- */
        bool btn = !gpio_read(BTN_PORT, BTN_PIN);
        if (btn && button_enabled && !inside_button_handler && button_def_pc != 0) {
            button_push_received  = 1;
            if (call_stack_ptr < 32) {
                inside_button_handler       = true;
                call_stack[call_stack_ptr++] = pc;
                pc = button_def_pc;
                continue;
            }
        }

        uint16_t word = get_word(pc);

        /* ---- ASSIGNMENT 0xe801 ---- */
        if (word == 0xe801) {
            uint16_t dest     = get_word(pc+1);
            uint16_t src1     = get_word(pc+2);
            uint16_t op_check = get_word(pc+3);
            if (op_check == 0) {
                write_var(dest, read_var(src1));
                pc += 4;
            } else {
                uint16_t op  = get_word(pc+4);
                write_var(dest, eval_op(op, read_var(src1), read_var(op_check)));
                pc += 5;
            }
            continue;
        }

        /* ---- IF 0xefef ---- */
        if (word == 0xefef) {
            uint16_t cond = get_word(pc+1);
            uint16_t bid  = get_word(pc+2);
            if (!read_var(cond)) {
                uint16_t s = pc+3;
                while (s < word_count) {
                    if (get_word(s)==0x1ff4 && get_word(s+1)==bid) { pc = s+2; break; }
                    s++;
                }
                if (s >= word_count) pc += 3;
            } else {
                pc += 3;
            }
            continue;
        }

        /* ---- END_IF 0x1ff4 ---- */
        if (word == 0x1ff4) { pc += 2; continue; }

        /* ---- GOTO 0xf8f8 ---- */
        if (word == 0xf8f8) {
            uint16_t t = get_word(pc+1);
            pc = (uint16_t)((t>>8) | ((t&0xFF)<<8));
            continue;
        }

        /* ---- FUNCTION CALL 0xf7f7 ---- */
        if (word == 0xf7f7) {
            if (call_stack_ptr < 32) {
                call_stack[call_stack_ptr++] = pc+2;
                uint16_t t = get_word(pc+1);
                pc = (uint16_t)((t>>8) | ((t&0xFF)<<8));
            } else {
                pc += 2;
            }
            continue;
        }

        /* ---- RETURN / END_FUNCTION 0xfdfd ---- */
        if (word == 0xfdfd) {
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc++;
            continue;
        }

        /* ---- KEY_DOWN / HOLD 0xfff8 ---- */
        if (word == 0xfff8) {
            uint16_t k = get_word(pc+1);
            send_keyboard_report((uint8_t)(k & 0xFF), (uint8_t)(k >> 8));
            pc += 2; continue;
        }

        /* ---- KEY_UP / RELEASE 0xeee8 ---- */
        if (word == 0xeee8) {
            send_keyboard_report(0, 0);
            pc += 2; continue;
        }

        /* ---- BUTTON_DEF 0xeaee — skip body during normal execution ---- */
        if (word == 0xeaee) {
            uint16_t bid = get_word(pc+1);
            uint16_t s   = pc+2;
            while (s < word_count) {
                if (get_word(s)==0xebf4 && get_word(s+1)==bid) { pc = s+2; break; }
                s++;
            }
            if (s >= word_count) pc += 2;
            continue;
        }

        /* ---- END_BUTTON 0xebf4 ---- */
        if (word == 0xebf4) {
            inside_button_handler = false;
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc += 2;
            continue;
        }

        /* ---- INJECT_VAR / RANDOM_* 0xe9e9 ---- */
        if (word == 0xe9e9) {
            uint16_t arg = get_word(pc+1);
            if      (arg==0xf442) type_char((char)get_random('a','z'));
            else if (arg==0xf542) type_char((char)get_random('A','Z'));
            else if (arg==0xf642) type_char(get_random(0,1) ? (char)get_random('a','z') : (char)get_random('A','Z'));
            else if (arg==0xf742) type_char((char)get_random('0','9'));
            else if (arg==0xf842) {
                const char sp[] = "!@#$%^&*()_+-=[]{}|;':\",./<>?~`";
                type_char(sp[get_random(0,(uint8_t)(sizeof(sp)-2))]);
            } else if (arg==0xf942) {
                type_char((char)get_random(32,126));
            } else {
                uint16_t v = read_var(arg);
                if (v>=32 && v<=126) type_char((char)v);
                else type_number((int)v);
            }
            pc += 2; continue;
        }

        /* ---- DELAY_VAR 0xe7e9 ---- */
        if (word == 0xe7e9) {
            delay_ms(read_var(get_word(pc+1)));
            pc += 2; continue;
        }

        /* ---- EXFIL_VAR 0xf6e9 (stub) ---- */
        if (word == 0xf6e9) { pc += 2; continue; }

        /* ---- ATTACKMODE opcodes ---- */
        if (word==0xf0f0 || word==0xf1f1 || word==0xf2f2 || word==0xf3f3) {
            parse_attackmode(&pc, word_count);
            apply_attackmode();
            continue;
        }


        /* ---- DELAY literal: high byte 0x00, low byte = ms ---- */
        if ((word >> 8) == 0x00) {
            delay_ms(word & 0xFF);
            pc++; continue;
        }

        /* ---- Builtins & raw key codes ---- */
        switch (word) {
            case 0x04ed: /* RESET — no-op in firmware; compiler inserts it */ break;

            case 0xebee: button_enabled = 0; break; /* DISABLE_BUTTON */
            case 0xecee: button_enabled = 1; break; /* ENABLE_BUTTON  */

            case 0xebf1: /* STOP_PAYLOAD */
                if (system_leds_enabled) {
                    gpio_low (LED_PORT_GREEN, LED_PIN_GREEN); /* green solid = done */
                    gpio_high(LED_PORT_RED,   LED_PIN_RED);
                }
                while (1) { usb_device_task(); }
                break;

            /* LED_OFF */
            case 0xeaed:
                led_off();
                break;

            /* LED_GREEN  (full name opcode) */
            case 0xebed:
                led_green_on();
                break;

            /* LED_G  (short alias opcode) */
            case 0xeced:
                led_green_on();
                break;

            /* LED_RED  (full name opcode) */
            case 0xeeed:
                led_red_on();
                break;

            /* LED_R  (short alias opcode) */
            case 0xeffe:
                led_red_on();
                break;

            case 0xedee: system_leds_enabled = 1; break; /* ENABLE_SYSTEM_LEDS  */
            case 0xeeee: system_leds_enabled = 0; break; /* DISABLE_SYSTEM_LEDS */

            case 0xeaeb: /* SAVE_HOST_KEYBOARD_LOCK_STATE */
                saved_caps_lock_on   = caps_lock_on;
                saved_num_lock_on    = num_lock_on;
                saved_scroll_lock_on = scroll_lock_on;
                break;

            case 0xebeb: /* RESTORE_HOST_KEYBOARD_LOCK_STATE */
                if (caps_lock_on   != saved_caps_lock_on)   { send_keyboard_report(0,0x39); delay_ms(10); send_keyboard_report(0,0); delay_ms(10); }
                if (num_lock_on    != saved_num_lock_on)    { send_keyboard_report(0,0x53); delay_ms(10); send_keyboard_report(0,0); delay_ms(10); }
                if (scroll_lock_on != saved_scroll_lock_on) { send_keyboard_report(0,0x47); delay_ms(10); send_keyboard_report(0,0); delay_ms(10); }
                break;

            case 0xeae9: /* SAVE_ATTACKMODE */
                saved_attackmode = current_attackmode;
                saved_vid        = current_vid;
                saved_pid        = current_pid;
                break;

            case 0xebe9: /* RESTORE_ATTACKMODE — restore saved state AND re-enumerate */
                current_attackmode = saved_attackmode;
                current_vid        = saved_vid;
                current_pid        = saved_pid;
                apply_attackmode();
                break;

            case 0x01ea: while (!caps_lock_on)   { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_CAPS_ON    */
            case 0x02ea: while ( caps_lock_on)   { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_CAPS_OFF   */
            case 0x03ea: {
                uint16_t s = caps_lock_on;
                while (caps_lock_on == s) { usb_device_task(); blink_green_tick(); }
                break; /* WAIT_FOR_CAPS_CHANGE */
            }
            case 0x04ea: while (!num_lock_on)    { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_NUM_ON     */
            case 0x05ea: while ( num_lock_on)    { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_NUM_OFF    */
            case 0x06ea: {
                uint16_t s = num_lock_on;
                while (num_lock_on == s) { usb_device_task(); blink_green_tick(); }
                break; /* WAIT_FOR_NUM_CHANGE */
            }
            case 0x07ea: while (!scroll_lock_on) { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_SCROLL_ON  */
            case 0x08ea: while ( scroll_lock_on) { usb_device_task(); blink_green_tick(); } break; /* WAIT_FOR_SCROLL_OFF */
            case 0x09ea: {
                uint16_t s = scroll_lock_on;
                while (scroll_lock_on == s) { usb_device_task(); blink_green_tick(); }
                break; /* WAIT_FOR_SCROLL_CHANGE */
            }

            case 0xeaea: /* WAIT_FOR_BUTTON_PRESS
                          * Just waits for the physical button — does NOT jump into
                          * BUTTON_DEF.  That was a bug in the original firmware. */
                button_push_received = 0;
                while (gpio_read(BTN_PORT, BTN_PIN)) {
                    usb_device_task();
                    blink_green_tick();
                }
                button_push_received = 1;
                delay_ms(20); /* debounce: wait for release */
                while (!gpio_read(BTN_PORT, BTN_PIN)) { usb_device_task(); }
                delay_ms(20);
                break;

            default: {
                /* Raw key opcode: high byte = HID keycode, low byte = modifier */
                uint8_t keycode  = (uint8_t)(word >> 8);
                uint8_t modifier = (uint8_t)(word & 0xFF);
                send_keyboard_report(modifier, keycode);
                delay_ms(char_delay_ms);
                send_keyboard_report(0, 0);
                delay_ms(char_delay_ms);
                break;
            }
        }
        pc++;
    } /* end main execution loop */

    g_payload_executing = false;
    /* Payload finished → green solid = idle */
    if (system_leds_enabled) {
        gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
        gpio_high(LED_PORT_RED,   LED_PIN_RED);
    }

    while (1) { usb_device_task(); }
    return 0;
}
