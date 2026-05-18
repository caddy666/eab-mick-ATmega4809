/**
 * @file  main.cpp
 * @brief CD drive firmware — Arduino entry point for ATmega4809.
 *
 * ── System overview ───────────────────────────────────────────────────────
 *
 * This firmware replaces the original 8051 MCU in the Commodore CD32 CD
 * drive (Philips/Commodore 1992–1993).  It runs on a custom ATmega4809 PCB
 * and implements the same command/status protocol seen by the Amiga chipset
 * (Akiko / Paula) over the COMMO 3-wire serial bus.
 *
 * ── Hardware interfaces ───────────────────────────────────────────────────
 *
 *   CXD2500BQ (CD6) — Sony CD signal processor: EFM decoder, CLV servo,
 *                     audio DAC.  Write-only 3-wire bit-bang SPI.
 *                     Pins: UCL (D14) / UDAT (D21) / ULAT (D20).
 *
 *   DSIC2           — Sony CXA1372 servo IC: focus, spindle, radial,
 *                     sledge.  NOT connected on this PCB revision.
 *
 *   Q-channel       — Subcode data clocked from CXD2500 via QDA (D13) /
 *                     QCL (D12), synchronised by SCOR falling-edge on D17.
 *
 *   COMMO bus       — 3-wire to Amiga chipset: DATA (D2) / CLK (D18) /
 *                     DIR (D19).
 *
 * ── Cooperative multitasking ──────────────────────────────────────────────
 *
 * There is no RTOS.  All modules are non-blocking state machines advancing
 * one step per loop() iteration:
 *
 *   1. command_handler()  — dispatch pending COMMO command to player
 *   2. COMMO_INTERFACE()  — step the serial RX/TX state machine
 *   3. Dispatcher()       — route packets and queue status updates
 *   4. player()           — advance disc-control sequence + background tasks
 *
 * ── Initialisation sequence ───────────────────────────────────────────────
 *
 *   player_init()           — timer_init(), driver_init(), reset_dsic2_cd6(),
 *                              servo_init(), cd6_init()
 *   COMMO_INIT()            — COMMO GPIO pins + state machine reset
 *   Init_command_handler()  — clear pending command slot
 *   enable_scor_counter()   — no-op; attachInterrupt done inside timer_init()
 */

#include <Arduino.h>

#include "defs.h"
#include "player.h"
#include "commo.h"
#include "cmd_hndl.h"
#include "driver.h"
#include "timer.h"
#include "gpio_map.h"

extern "C" void Dispatcher(void);

/* =========================================================================
 * setup — one-time hardware and subsystem initialisation
 * ====================================================================== */

void setup(void)
{
    /* player_init() performs the full hardware bring-up sequence:
     *   timer_init()      — start TCB1 8 ms tick + SCOR interrupt
     *   driver_init()     — configure all GPIO pins
     *   reset_dsic2_cd6() — assert reset, wait, release
     *   servo_init()      — set servo state machine to INIT_DSIC2
     *   cd6_init()        — send CXD2500 startup register sequence */
    player_init();

    COMMO_INIT();
    Init_command_handler();
    enable_scor_counter();   /* no-op; SCOR ISR attached in timer_init() */

    digitalWrite(PIN_LED_STATUS, HIGH);   /* signal boot complete */
}

/* =========================================================================
 * loop — cooperative multitasking main loop
 * ====================================================================== */

void loop(void)
{
    command_handler();    /* 1. dispatch pending command to player   */
    COMMO_INTERFACE();    /* 2. service serial RX/TX state machine   */
    Dispatcher();         /* 3. route packets and status updates     */
    player();             /* 4. advance disc control + background    */
}
