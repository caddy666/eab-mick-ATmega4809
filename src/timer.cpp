/**
 * @file  timer.cpp
 * @brief Software timer subsystem and SCOR interrupt — ATmega4809 Arduino port.
 *
 * ── Software timers ───────────────────────────────────────────────────────
 *
 * TCB1 fires every 8 ms and decrements all non-zero entries in timers[].
 * At 16 MHz ÷ 2 = 8 MHz, CCMP = 63 999 gives exactly 64 000 cycles = 8 ms.
 * TCA0 is reserved by MegaCoreX for millis() / PWM; TCB1 is free.
 *
 * ── SCOR interrupt ────────────────────────────────────────────────────────
 *
 * PIN_SCOR (D17 / PD0) falling edge fires at 75 Hz per CD frame.  The ISR
 * sets scor_edge = 1 for subcode_module() and decrements scor_counter for
 * frame-accurate seek timing in servo.cpp.
 *
 * ── blocking_delay() ──────────────────────────────────────────────────────
 *
 * Set delay_byte to a count and call blocking_delay() to spin for
 * delay_byte × 500 µs.  Named blocking_delay (not delay) to avoid colliding
 * with Arduino's delay(unsigned long ms).  Only used during IC reset
 * sequencing in driver.cpp where a blocking wait is acceptable.
 */

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

#include "timer.h"
#include "gpio_map.h"

/* =========================================================================
 * Software timer array
 * ====================================================================== */
volatile uint8_t timers[TIMER_COUNT] = {0};

/* =========================================================================
 * SCOR interrupt state
 * ====================================================================== */
volatile uint8_t scor_counter = 0;
volatile uint8_t scor_edge    = 0;

/* =========================================================================
 * Legacy blocking-delay counter
 * ====================================================================== */
volatile uint8_t delay_byte = 0;

/* =========================================================================
 * TCB1 ISR — fires every 8 ms, decrements all non-zero software timers
 * ====================================================================== */
ISR(TCB1_INT_vect)
{
    TCB1.INTFLAGS = TCB_CAPT_bm;   /* clear interrupt flag (write 1 to clear) */
    for (int i = 0; i < TIMER_COUNT; i++) {
        if (timers[i]) timers[i]--;
    }
}

/* =========================================================================
 * SCOR falling-edge ISR — one CD frame boundary (75 Hz)
 * ====================================================================== */
static void scor_isr(void)
{
    scor_edge = 1;
    if (scor_counter > 0)
        scor_counter--;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

/**
 * @brief  Initialise the 8 ms TCB1 periodic timer and SCOR GPIO interrupt.
 *
 * Called once from player_init().  PIN_SCOR must already be configured as
 * INPUT_PULLUP by driver_init() before this function is called.
 */
void timer_init(void)
{
    /* TCB1 periodic interrupt mode at 8 ms.
     * Clock source: CLK_PER / 2 = 8 MHz.  Period = 64 000 cycles → CCMP = 63 999. */
    TCB1.CCMP    = 63999;
    TCB1.CTRLB   = TCB_CNTMODE_INT_gc;
    TCB1.INTCTRL = TCB_CAPT_bm;
    TCB1.CTRLA   = TCB_CLKSEL_CLKDIV2_gc | TCB_ENABLE_bm;

    /* SCOR falling-edge interrupt */
    attachInterrupt(digitalPinToInterrupt(PIN_SCOR), scor_isr, FALLING);
}

/**
 * @brief  Blocking delay — approximately delay_byte × 500 µs.
 *
 * Renamed from delay() to avoid the Arduino delay(unsigned long) collision.
 */
void blocking_delay(void)
{
    /* while-loop (not do-while) avoids executing once when delay_byte == 0. */
    while (delay_byte--) {
        delayMicroseconds(500);
    }
}

/**
 * @brief  Blocking delay of n × 500 µs.
 * @param  n  Number of 500 µs periods to wait.
 */
void delay_us_500x(uint8_t n)
{
    /* Use uint32_t product: (unsigned int) is 16-bit on AVR and overflows
     * for n > 131 (132 * 500 = 66 000 > 65 535). */
    delayMicroseconds((uint32_t)n * 500u);
}
