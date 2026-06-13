#include <stdint.h>
#include <avr32/io.h>

/*
 * diagnose.c - LED diagnostic blink patterns
 * Provides visual feedback for fatal errors.
 */

/* Simple delay loop */
static void delay_cycles(uint32_t cycles) {
    while(cycles--) {
        __asm__ __volatile__("nop");
    }
}

/* 
 * Blinks the red LED `code` times, pauses, then repeats.
 * Assumes LED is connected to a specific GPIO pin.
 */
void error_blink(int code) {
    // Assuming LED is on PA13 (0) or similar.
    // Configure pin as output if not already done.
    AVR32_GPIO.port[0].oders = (1 << 13);
    AVR32_GPIO.port[0].gpers = (1 << 13);
    
    while(1) {
        for(int i = 0; i < code; i++) {
            AVR32_GPIO.port[0].ovrs = (1 << 13); // LED ON
            delay_cycles(1000000);
            AVR32_GPIO.port[0].ovrc = (1 << 13); // LED OFF
            delay_cycles(1000000);
        }
        delay_cycles(4000000); // Long pause
    }
}