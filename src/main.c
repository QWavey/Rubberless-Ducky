/**
 * @file main.c
 * @brief Custom HID Firmware for AT32UC3B1 (AVR32 UC3B1256) with PetitFS Payload
 */

#include <stdint.h>
#include <avr32/io.h>
#include <string.h>
#include "usb_hid.h"
#include "usb_descriptors.h"
#include "pff.h"

/* Custom basic string functions for freestanding/nostdlib environment */
void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    char *p = s;
    while (n--) {
        *p++ = (char)c;
    }
    return s;
}

#define Enable_global_interrupt()  __asm__ __volatile__ ("csrf 16")
#define Disable_global_interrupt() __asm__ __volatile__ ("ssrf 16")

#define CYCLES_PER_MS  12000u

#define LED_PORT_GREEN 0       /* PORTA */
#define LED_PIN_GREEN  7       /* PA07 (Green) */
#define LED_PORT_RED   0       /* PORTA */
#define LED_PIN_RED    8       /* PA08 (Red) */
#define BTN_PORT       0       /* PORTA */
#define BTN_PIN        13      /* PA13  */

#define _GPIO_PORT(n)  ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void gpio_out(int port, int pin) {
    volatile avr32_gpio_port_t *p = _GPIO_PORT(port);
    p->gpers = (1u << pin);
    p->oders = (1u << pin);
}

static inline void gpio_in_pullup(int port, int pin) {
    volatile avr32_gpio_port_t *p = _GPIO_PORT(port);
    p->gpers = (1u << pin);
    p->oderc = (1u << pin);
    p->puers = (1u << pin);
}

static inline void gpio_high(int port, int pin) { _GPIO_PORT(port)->ovrs = (1u << pin); }
static inline void gpio_low (int port, int pin) { _GPIO_PORT(port)->ovrc = (1u << pin); }
static inline bool gpio_read(int port, int pin) { return !!(_GPIO_PORT(port)->pvr & (1u << pin)); }

typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} keyboard_report_t;

static volatile bool g_usb_ready = false;

static uint32_t host_configuration_request_count = 0;

void usb_device_enumerated_cb(void) {
    g_usb_ready = true;
}

void usb_device_config_requested_cb(void) {
    host_configuration_request_count++;
}

static uint16_t variables[1024];
static uint16_t button_enabled = 1;
static uint16_t button_user_defined = 0;
static uint16_t button_push_received = 0;
static uint16_t system_leds_enabled = 1;
static uint16_t injecting_leds_enabled = 1;
static uint16_t caps_lock_on = 0;
static uint16_t num_lock_on = 0;
static uint16_t scroll_lock_on = 0;
static uint16_t saved_caps_lock_on = 0;
static uint16_t saved_num_lock_on = 0;
static uint16_t saved_scroll_lock_on = 0;
static uint16_t button_timeout = 0;
static uint16_t payload_parse_speed = 0;
uint16_t current_vid = 0x0481;
uint16_t current_pid = 0x0001;
static uint16_t current_os = 1; // WINDOWS
uint8_t current_attackmode = 0;
static uint16_t jitter_enabled = 0;
static uint16_t jitter_max = 0;
static uint16_t led_show_caps = 0;
static uint16_t led_show_num = 0;
static uint16_t led_show_scroll = 0;
static uint16_t storage_leds_enabled = 1;
static uint16_t led_continuous_show_storage_activity = 0;
static uint16_t storage_activity_timeout = 1000;
static uint16_t received_host_lock_led_reply = 0;
static uint16_t exfil_leds_enabled = 1;
static uint16_t exfil_mode_enabled = 0;

static uint16_t saved_attackmode = 1;
static uint16_t saved_vid = 0x0481;
static uint16_t saved_pid = 0x0001;

static uint16_t call_stack[32];
static uint8_t call_stack_ptr = 0;
static uint16_t button_def_pc = 0;
static bool inside_button_handler = false;

void usb_hid_report_out_cb(uint8_t *data, uint8_t length) {
    if (length > 0) {
        uint8_t leds = data[0];
        num_lock_on = !!(leds & 0x01);
        caps_lock_on = !!(leds & 0x02);
        scroll_lock_on = !!(leds & 0x04);
        received_host_lock_led_reply = 1;
        
        // Auto show lock keys on Ducky's LEDs if $_LED_SHOW_* is active
        if (led_show_caps && caps_lock_on) {
            gpio_low(LED_PORT_GREEN, LED_PIN_GREEN);
        } else if (led_show_caps) {
            gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
        }
        if (led_show_num && num_lock_on) {
            gpio_low(LED_PORT_RED, LED_PIN_RED);
        } else if (led_show_num) {
            gpio_high(LED_PORT_RED, LED_PIN_RED);
        }
        
        /* Toggle Red LED if Caps Lock is ON. Note: Red LED is also used for SD errors. */
        if (!led_show_caps && !led_show_num) {
            if (leds & 0x02) gpio_low(LED_PORT_RED, LED_PIN_RED);
            else             gpio_high(LED_PORT_RED, LED_PIN_RED);
        }
    }
}

#include <avr32/io.h>

static inline uint32_t get_cpu_count(void) {
    return __builtin_mfsr(AVR32_COUNT);
}

static void send_keyboard_report(uint8_t modifier, uint8_t keycode) {
    keyboard_report_t report;
    memset(&report, 0, sizeof(report));
    report.modifier = modifier;
    report.keys[0] = keycode;
    
    /* TRUE BLIND TYPING: If Windows is still loading the generic driver,
     * keystrokes are dropped. This perfectly matches the original USB 
     * Rubber Ducky behavior and forces hardware delay to 0.0s. */
    if (!g_usb_ready) return;
    
    /* FASTEST TYPING SPEED: Wait for the USB FIFO to empty so we don't
     * drop keys during normal typing, but timeout after 5ms (240,000 cycles) 
     * so we don't block if the host stops polling. */
    uint32_t timeout_start = get_cpu_count();
    while (!usb_hid_in_endpoint_ready()) {
        if ((get_cpu_count() - timeout_start) > 240000) {
            return;
        }
        usb_device_task();
    }

    usb_hid_send_report((uint8_t*)&report, sizeof(report));
}

static void delay_ms(uint32_t ms) {
    while (ms--) {
        uint32_t start = get_cpu_count();
        /* 48,000 cycles = 1 millisecond at 48 MHz */
        while ((get_cpu_count() - start) < 48000) {
            usb_device_task();
        }
    }
}

#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08

#define KEY_R       0x15
#define KEY_ENTER   0x28

static void type_char(char c) {
    uint8_t key = 0;
    if (c >= 'a' && c <= 'z') key = c - 'a' + 0x04;
    else if (c >= 'A' && c <= 'Z') key = c - 'A' + 0x04;
    else if (c >= '1' && c <= '9') key = c - '1' + 0x1E;
    else if (c == '0') key = 0x27;
    else if (c == ' ') key = 0x2C;
    else if (c == '\n') key = 0x28;
    else if (c == '.') key = 0x37;
    else if (c == ':') key = 0x33;

    if (key) {
        uint8_t mod = ((c >= 'A' && c <= 'Z') || c == ':') ? MOD_LSHIFT : 0;
        send_keyboard_report(mod, key);
        delay_ms(2);
        send_keyboard_report(0, 0);
        delay_ms(2);
    }
}

static void type_string(const char *str) {
    while (*str) type_char(*str++);
}

static void type_number(int n) {
    if (n >= 100) {
        type_char('0' + (n / 100));
        n %= 100;
        if (n < 10) type_char('0');
    }
    if (n >= 10) {
        type_char('0' + (n / 10));
    }
    type_char('0' + (n % 10));
}

static uint8_t payload_ram[8192];

static uint16_t get_word(uint16_t idx) {
    uint16_t byte_idx = idx * 2;
    return ((uint16_t)payload_ram[byte_idx] << 8) | payload_ram[byte_idx + 1];
}

static uint16_t read_var(uint16_t addr) {
    if (addr >= 0x0001 && addr < 1024) {
        return variables[addr];
    }
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
        case 0xf042: return 0; // $_RANDOM_MIN (default 0)
        case 0xf142: return 9; // $_RANDOM_MAX (default 9)
        case 0x6742: return 0; // FALSE
        case 0x6842: return 1; // TRUE
        case 0x6942: return 1; // WINDOWS
        case 0x7042: return 2; // MACOS
        case 0x7142: return 3; // LINUX
        case 0x7242: return 4; // ANDROID
        case 0x7342: return 5; // IOS
        case 0x7442: return 6; // CHROMEOS
        default: return 0;
    }
}

static void write_var(uint16_t addr, uint16_t val) {
    if (addr >= 0x0001 && addr < 1024) {
        variables[addr] = val;
        return;
    }
    switch (addr) {
        case 0x8042: button_enabled = val; break;
        case 0x8142: button_user_defined = val; break;
        case 0x8442: button_push_received = val; break;
        case 0x8542: led_continuous_show_storage_activity = val; break;
        case 0x8642: system_leds_enabled = val; break;
        case 0x8742: storage_leds_enabled = val; break;
        case 0x8842: injecting_leds_enabled = val; break;
        case 0x8942: exfil_leds_enabled = val; break;
        case 0x9042: caps_lock_on = val; break;
        case 0x9142: num_lock_on = val; break;
        case 0x9242: scroll_lock_on = val; break;
        case 0x9342: saved_caps_lock_on = val; break;
        case 0x9442: saved_num_lock_on = val; break;
        case 0x9542: saved_scroll_lock_on = val; break;
        case 0x9642: received_host_lock_led_reply = val; break;
        case 0x9742: exfil_mode_enabled = val; break;
        case 0x9842: storage_activity_timeout = val; break;
        case 0x9942: button_timeout = val; break;
        case 0x9A42: payload_parse_speed = val; break;
        case 0x9B42: current_vid = val; break;
        case 0x9C42: current_pid = val; break;
        case 0x9D42: current_os = val; break;
        case 0x9F42: host_configuration_request_count = val; break;
        case 0xA042: current_attackmode = val; break;
        case 0xA242: jitter_enabled = val; break;
        case 0xA342: jitter_max = val; break;
    }
}

static uint16_t eval_op(uint16_t op, uint16_t val1, uint16_t val2) {
    switch (op) {
        case 0xe802: return val1 + val2;
        case 0xe803: return val1 - val2;
        case 0xe804: return val1 * val2;
        case 0xe805: return val2 != 0 ? val1 / val2 : 0;
        case 0xe806: return val1 == val2;
        case 0xe807: return val1 != val2;
        case 0xe808: return val1 < val2;
        case 0xe809: return val1 > val2;
        case 0xe8a8: return val1 <= val2;
        case 0xe8a9: return val1 >= val2;
        case 0xe8aa: return val1 && val2;
        case 0xe8bb: return val1 || val2;
        case 0xe80a: return val1 & val2;
        case 0xe80b: return val1 | val2;
        case 0xe80c: return val1 >> val2;
        case 0xe80d: return val1 << val2;
        case 0xe80e: return val2 != 0 ? val1 % val2 : 0;
        case 0x0fe8: {
            uint16_t res = 1;
            for (uint16_t i = 0; i < val2; i++) res *= val1;
            return res;
        }
        default: return 0;
    }
}

static uint32_t rand_seed = 12345;
static uint8_t get_random(uint8_t min, uint8_t max) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return min + ((rand_seed / 65536) % (max - min + 1));
}

FATFS fs;


static void start_usb_and_wait(void) {
    usb_device_init();
    usb_device_register_callback(USB_EVENT_ENUMERATED, usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);
    usb_device_enable();
    for (int i = 0; i < 200; i++) {
        uint32_t start = get_cpu_count();
        while ((get_cpu_count() - start) < 48000) {}
    }
}

int main(void)
{
    Disable_global_interrupt();

    volatile uint32_t *wdt_ctrl = (volatile uint32_t *)(&AVR32_WDT.ctrl);
    uint32_t current_ctrl = *wdt_ctrl & ~AVR32_WDT_CTRL_KEY_MASK;
    *wdt_ctrl = current_ctrl | (0x55ul << AVR32_WDT_CTRL_KEY_OFFSET);
    *wdt_ctrl = (current_ctrl & ~AVR32_WDT_CTRL_EN_MASK) | (0xAAul << AVR32_WDT_CTRL_KEY_OFFSET);

    /* Switch to internal RC oscillator first to prevent emergency fallback */
    AVR32_PM.mcctrl = 0; /* 0 is RCSYS */
    
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
    AVR32_PM.mcctrl = AVR32_PM_MCCTRL_OSC0EN_MASK | (AVR32_PM_MCCTRL_MCSEL_PLL0 << AVR32_PM_MCCTRL_MCSEL_OFFSET);

    gpio_out       (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high      (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_out       (LED_PORT_RED, LED_PIN_RED);
    gpio_high      (LED_PORT_RED, LED_PIN_RED);
    gpio_in_pullup (BTN_PORT, BTN_PIN);



    FRESULT res_mount = pf_mount(&fs);
    if (res_mount == FR_OK) {
        FRESULT res_bin = pf_open("INJECT.BIN");
        
        if (res_bin == FR_OK) {
            gpio_low(LED_PORT_GREEN, LED_PIN_GREEN); // Green ON -> OK
            
            UINT br;
            FRESULT read_res = pf_read(payload_ram, sizeof(payload_ram), &br);
            if (read_res == FR_OK && br > 0) {
                uint16_t word_count = br / 2;
                
                // Pre-scan for BUTTON_DEF
                for (uint16_t i = 0; i < word_count; i++) {
                    if (get_word(i) == 0xeaee) {
                        button_def_pc = i + 2;
                    }
                }
                
                // Parse variable block if present
                uint16_t pc = 0;
                if (word_count > 0 && get_word(0) == 0xe8e8) {
                    uint16_t var_idx = 1;
                    pc = 1;
                    while (pc < word_count && get_word(pc) != 0xe8e8) {
                        if (var_idx < 1024) {
                            variables[var_idx++] = get_word(pc);
                        }
                        pc++;
                    }
                    if (pc < word_count) pc++; // Skip closing 0xe8e8
                }
                
                start_usb_and_wait();
                // Execution loop
                while (pc < word_count) {
                    usb_device_task();
                    
                    // Check button press if enabled and not already inside handler
                    bool btn_pressed = !gpio_read(BTN_PORT, BTN_PIN);
                    if (btn_pressed && button_enabled && !inside_button_handler) {
                        button_push_received = 1;
                        if (button_def_pc != 0) {
                            inside_button_handler = true;
                            if (call_stack_ptr < 32) {
                                call_stack[call_stack_ptr++] = pc;
                                pc = button_def_pc;
                                continue;
                            }
                        }
                    }
                    
                    uint16_t word = get_word(pc);
                    
                    if (word == 0xe801) { // ASSIGNMENT
                        uint16_t dest = get_word(pc + 1);
                        uint16_t src1 = get_word(pc + 2);
                        uint16_t op_check = get_word(pc + 3);
                        if (op_check == 0) {
                            write_var(dest, read_var(src1));
                            pc += 4;
                        } else {
                            uint16_t op = get_word(pc + 4);
                            uint16_t val1 = read_var(src1);
                            uint16_t val2 = read_var(op_check);
                            write_var(dest, eval_op(op, val1, val2));
                            pc += 5;
                        }
                    } else if (word == 0xefef) { // IF
                        uint16_t cond = get_word(pc + 1);
                        uint16_t bid = get_word(pc + 2);
                        if (!read_var(cond)) {
                            uint16_t scan = pc + 3;
                            while (scan < word_count) {
                                if (get_word(scan) == 0x1ff4 && get_word(scan + 1) == bid) {
                                    pc = scan + 2;
                                    break;
                                }
                                scan++;
                            }
                            if (scan >= word_count) {
                                pc += 3;
                            }
                        } else {
                            pc += 3;
                        }
                    } else if (word == 0x1ff4) { // END_IF
                        pc += 2;
                    } else if (word == 0xf8f8) { // GOTO
                        uint16_t target = get_word(pc + 1);
                        pc = (target >> 8) | ((target & 0xFF) << 8);
                    } else if (word == 0xf7f7) { // FUNCTION CALL
                        if (call_stack_ptr < 32) {
                            call_stack[call_stack_ptr++] = pc + 2;
                            uint16_t target = get_word(pc + 1);
                            pc = (target >> 8) | ((target & 0xFF) << 8);
                        } else {
                            pc += 2;
                        }
                    } else if (word == 0xfdfd) { // RETURN
                        if (call_stack_ptr > 0) {
                            pc = call_stack[--call_stack_ptr];
                        } else {
                            pc++;
                        }
                    } else if (word == 0xfff8) { // HOLD
                        uint16_t key = get_word(pc + 1);
                        send_keyboard_report(key & 0xFF, key >> 8);
                        pc += 2;
                    } else if (word == 0xeee8) { // RELEASE
                        send_keyboard_report(0, 0);
                        pc += 2;
                    } else if (word == 0xeaee) { // BUTTON_DEF (skip in normal execution)
                        uint16_t bid = get_word(pc + 1);
                        uint16_t scan = pc + 2;
                        while (scan < word_count) {
                            if (get_word(scan) == 0xebf4 && get_word(scan + 1) == bid) {
                                pc = scan + 2;
                                break;
                            }
                            scan++;
                        }
                        if (scan >= word_count) {
                            pc += 2;
                        }
                    } else if (word == 0xebf4) { // END_BUTTON
                        inside_button_handler = false;
                        if (call_stack_ptr > 0) {
                            pc = call_stack[--call_stack_ptr];
                        } else {
                            pc += 2;
                        }
                    } else if (word == 0xe9e9) { // INJECT_VAR / RANDOM_*
                        uint16_t arg = get_word(pc + 1);
                        if (arg == 0xf442) {
                            type_char((char)get_random('a', 'z'));
                        } else if (arg == 0xf542) {
                            type_char((char)get_random('A', 'Z'));
                        } else if (arg == 0xf642) {
                            type_char(get_random(0, 1) ? (char)get_random('a', 'z') : (char)get_random('A', 'Z'));
                        } else if (arg == 0xf742) {
                            type_char((char)get_random('0', '9'));
                        } else if (arg == 0xf842) {
                            const char special[] = "!@#$%^&*()_+-=[]{}|;':\",./<>?~`";
                            type_char(special[get_random(0, sizeof(special) - 2)]);
                        } else if (arg == 0xf942) {
                            type_char((char)get_random(32, 126));
                        } else {
                            uint16_t val = read_var(arg);
                            if (val >= 32 && val <= 126) {
                                type_char((char)val);
                            } else {
                                type_number(val);
                            }
                        }
                        pc += 2;
                    } else if (word == 0xe7e9) { // DELAY_VAR
                        uint16_t arg = get_word(pc + 1);
                        delay_ms(read_var(arg));
                        pc += 2;
                    } else if (word == 0xf6e9) { // EXFIL_VAR
                        pc += 2;
                    } else if (word == 0xf0f0 || word == 0xf1f1 || word == 0xf2f2 || word == 0xf3f3) { // ATTACKMODE
                        if (word == 0xf0f0) current_attackmode = 0; // OFF
                        else if (word == 0xf1f1) current_attackmode = 1; // HID
                        else if (word == 0xf2f2) current_attackmode = 2; // STORAGE
                        else if (word == 0xf3f3) current_attackmode = 3; // HID STORAGE
                        
                        pc++;
                        while (pc < word_count && get_word(pc) != word) {
                            uint16_t param = get_word(pc);
                            if (param == 0xf5f5) { // VID
                                current_vid = get_word(pc + 1);
                                pc += 2;
                            } else if (param == 0xf6f6) { // PID
                                current_pid = get_word(pc + 1);
                                pc += 2;
                            } else {
                                pc++;
                            }
                        }
                        if (pc < word_count) pc++;
                        
                        usb_device_disable();
                        received_host_lock_led_reply = 0;
                        delay_ms(250);
                        if (current_attackmode == 2) {
                            usb_config_descriptor = usb_config_descriptor_msc;
                            usb_config_descriptor_size = usb_config_descriptor_msc_size;
                        } else if (current_attackmode == 3) {
                            usb_config_descriptor = usb_config_descriptor_comp;
                            usb_config_descriptor_size = usb_config_descriptor_comp_size;
                        } else {
                            usb_config_descriptor = usb_config_descriptor_hid;
                            usb_config_descriptor_size = usb_config_descriptor_hid_size;
                        }
                        
                        // Dynamically update VID/PID if changed, and change PID for composite devices to bypass OS caching
                        extern uint8_t usb_device_descriptor[];
                        uint16_t pid = current_pid;
                        if (current_attackmode == 3) pid ^= 0x4242; // Change PID for HID+STORAGE to force re-enumeration
                        else if (current_attackmode == 2) pid ^= 0x1337; // Change PID for STORAGE ONLY
                        
                        usb_device_descriptor[8] = current_vid & 0xFF;
                        usb_device_descriptor[9] = current_vid >> 8;
                        usb_device_descriptor[10] = pid & 0xFF;
                        usb_device_descriptor[11] = pid >> 8;
                        
                        if (current_attackmode != 0) {
                            usb_device_enable();
                        }
                    } else if ((word >> 8) == 0x00) { // DELAY Literal
                        delay_ms(word & 0xFF);
                        pc++;
                    } else { // Builtins & Keys
                        switch (word) {
                            case 0x04ed: // RESET
                                break;
                            case 0xebee: // DISABLE_BUTTON
                                button_enabled = 0;
                                break;
                            case 0xecee: // ENABLE_BUTTON
                                button_enabled = 1;
                                break;
                            case 0xebf1: // STOP_PAYLOAD
                                while (1) usb_device_task();
                                break;
                            case 0xeaed: // LED_OFF
                                gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
                                gpio_high(LED_PORT_RED, LED_PIN_RED);
                                break;
                            case 0xebed: // LED_GREEN
                                gpio_low(LED_PORT_GREEN, LED_PIN_GREEN);
                                gpio_high(LED_PORT_RED, LED_PIN_RED);
                                break;
                            case 0xeced: // LED_RED
                                gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
                                gpio_low(LED_PORT_RED, LED_PIN_RED);
                                break;
                            case 0xeaeb: // SAVE_HOST_KEYBOARD_LOCK_STATE
                                saved_caps_lock_on = caps_lock_on;
                                saved_num_lock_on = num_lock_on;
                                saved_scroll_lock_on = scroll_lock_on;
                                break;
                            case 0xebeb: // RESTORE_HOST_KEYBOARD_LOCK_STATE
                                if (caps_lock_on != saved_caps_lock_on) {
                                    send_keyboard_report(0, 0x39);
                                    send_keyboard_report(0, 0);
                                }
                                if (num_lock_on != saved_num_lock_on) {
                                    send_keyboard_report(0, 0x53);
                                    send_keyboard_report(0, 0);
                                }
                                if (scroll_lock_on != saved_scroll_lock_on) {
                                    send_keyboard_report(0, 0x47);
                                    send_keyboard_report(0, 0);
                                }
                                break;
                            case 0xeae9: // SAVE_ATTACKMODE
                                saved_attackmode = current_attackmode;
                                saved_vid = current_vid;
                                saved_pid = current_pid;
                                break;
                            case 0xebe9: // RESTORE_ATTACKMODE
                                current_attackmode = saved_attackmode;
                                current_vid = saved_vid;
                                current_pid = saved_pid;
                                break;
                            case 0x01ea: // WAIT_FOR_CAPS_ON
                                while (!caps_lock_on) usb_device_task();
                                break;
                            case 0x02ea: // WAIT_FOR_CAPS_OFF
                                while (caps_lock_on) usb_device_task();
                                break;
                            case 0x03ea: { // WAIT_FOR_CAPS_CHANGE
                                uint16_t start = caps_lock_on;
                                while (caps_lock_on == start) usb_device_task();
                                break;
                            }
                            case 0x04ea: // WAIT_FOR_NUM_ON
                                while (!num_lock_on) usb_device_task();
                                break;
                            case 0x05ea: // WAIT_FOR_NUM_OFF
                                while (num_lock_on) usb_device_task();
                                break;
                            case 0x06ea: { // WAIT_FOR_NUM_CHANGE
                                uint16_t start = num_lock_on;
                                while (num_lock_on == start) usb_device_task();
                                break;
                            }
                            case 0x07ea: // WAIT_FOR_SCROLL_ON
                                while (!scroll_lock_on) usb_device_task();
                                break;
                            case 0x08ea: // WAIT_FOR_SCROLL_OFF
                                while (scroll_lock_on) usb_device_task();
                                break;
                            case 0x09ea: { // WAIT_FOR_SCROLL_CHANGE
                                uint16_t start = scroll_lock_on;
                                while (scroll_lock_on == start) usb_device_task();
                                break;
                            }
                            case 0xeaea: // WAIT_FOR_BUTTON_PRESS
                                while (gpio_read(BTN_PORT, BTN_PIN)) {
                                    usb_device_task();
                                }
                                button_push_received = 1;
                                if (button_def_pc != 0) {
                                    if (call_stack_ptr < 32) {
                                        call_stack[call_stack_ptr++] = pc + 1;
                                        pc = button_def_pc;
                                        continue;
                                    }
                                }
                                break;
                            default: {
                                uint8_t keycode = word >> 8;
                                uint8_t modifier = word & 0xFF;
                                send_keyboard_report(modifier, keycode);
                                delay_ms(2);
                                send_keyboard_report(0, 0);
                                delay_ms(2);
                                break;
                            }
                        }
                        pc++;
                    }
                }
            }
        } else {
            gpio_low(LED_PORT_RED, LED_PIN_RED); // Red ON
            start_usb_and_wait();
            type_string("failed to open INJECT.BIN. error: ");
            type_number(res_bin);
            type_char('\n');
        }
    } else {
        gpio_low(LED_PORT_RED, LED_PIN_RED); // Red ON
        start_usb_and_wait();
        type_string("failed to mount sd card. error: ");
        type_number(res_mount);
        type_char('\n');
    }

    while (1) usb_device_task();

    return 0;
}
