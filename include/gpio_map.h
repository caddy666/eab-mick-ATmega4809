/**
 * @file  gpio_map.h
 * @brief GPIO pin assignments for the ATmega4809 CD drive firmware.
 *
 * Maps the legacy Pico-era symbolic names (PIN_CXD_CLK, PIN_COMMO_DIR, etc.)
 * used throughout driver.c / commo.c onto the ATmega4809 Arduino D-numbers
 * from pins.h.
 *
 * Include gpio_map.h in all driver code; do not include pins.h directly.
 *
 * Pins defined as (-1) are not routed on this PCB revision:
 *   PIN_DSIC_CLK / PIN_DSIC_DATA / PIN_DSIC_LAT — DSIC2 bus (SICL/SIDA/SILD)
 *   PIN_HF_DET — HF (disc presence) detector, no dedicated header pin
 *   PIN_SENS   — CXD2500BQ SENS, routed to AREF/PD7 (no standard D-number)
 *
 * driver.cpp guards every code path that would write to a (-1) pin with
 * #if PIN_xxx >= 0, so those paths are compiled out entirely.
 */

#pragma once
#include "pins.h"   /* canonical ATmega4809 D-number assignments */

/* ── CXD2500BQ control bus aliases (Sony: UCL/UDAT/ULAT) ────────────────── */
#define PIN_CXD_CLK    PIN_UCL    /**< D14 / PD3 — serial clock  (CLOK) */
#define PIN_CXD_DATA   PIN_UDAT   /**< D21 / PD5 — serial data   (DATA) */
#define PIN_CXD_LAT    PIN_ULAT   /**< D20 / PD4 — latch strobe  (XLAT) */

/* ── DSIC2 servo IC bus aliases (Sony: SICL/SIDA/SILD) ─────────────────── *
 * NOT brought out to the ATmega4809 PCB header.  All three are (-1).        *
 * Every DSIC2 code path in driver.cpp is guarded: #if PIN_DSIC_CLK >= 0    */
#define PIN_DSIC_CLK   PIN_SICL   /**< (-1) — not connected */
#define PIN_DSIC_DATA  PIN_SIDA   /**< (-1) — not connected */
#define PIN_DSIC_LAT   PIN_SILD   /**< (-1) — not connected */

/* ── COMMO bus aliases (Commodore CD32 host protocol) ───────────────────── */
#define PIN_COMMO_DIR  PIN_IF_DIR   /**< D19 / PA3 — direction control output */
#define PIN_COMMO_CLK  PIN_IF_CLK   /**< D18 / PA2 — serial clock             */
#define PIN_COMMO_DATA PIN_IF_DATA  /**< D2  / PA0 — bidirectional data        */

/* ── Status output ──────────────────────────────────────────────────────── */
#define PIN_LED_STATUS PIN_ACTIVE   /**< D10 / PB1 — driven HIGH after boot    */

/* ── Pins not routed on this PCB revision ──────────────────────────────── */

/** HF (disc-presence) detector: no dedicated header pin on this board.
 *  hf_present() returns 1 (disc assumed present) when this is (-1).        */
#define PIN_HF_DET  (-1)

/** CXD2500BQ SENS bidirectional status pin.
 *  Physically connected to AREF/PD7 — no standard D-number in MegaCoreX.
 *  sens_read() returns 0 and write paths are compiled out.                  */
#define PIN_SENS    (-1)

/* Note: PIN_QDA, PIN_QCL, PIN_DOOR, PIN_SCOR, PIN_GFS, PIN_MUTE, PIN_FOK
 * are already defined in pins.h with the correct ATmega4809 D-numbers.      */
