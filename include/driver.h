/**
 * @file  driver.h
 * @brief Hardware driver API: CXD2545Q (CD6), DSIC2 servo IC, subcode reader,
 *        analogue I/O sense, SCOR counter, and brake-table helpers.
 *
 * ── CXD2545Q (referred to as "CD6" throughout the source) ────────────────
 *
 * Sony's CD signal processor IC with integrated digital servo.  Controls:
 *   - EFM decoder and CIRC error correction
 *   - CLV (constant linear velocity) spindle servo loop
 *   - Audio DAC output (mute/attenuate via Register A: $AX)
 *   - Subcode serial output (QDA/QCL/SCOR)
 *
 * Interface: write-only, 3-wire bit-bang SPI (UCL/UDAT/ULAT).
 * Protocol: 8 data bits LSB-first, then latch (no dummy clocks).
 * High-level commands are expressed as symbolic constants (MOT_*, MUTE, etc.)
 * and translated to CXD2545Q command bytes inside cd6_wr().
 *
 * ── DSIC2 (Sony CXA1372 servo IC) ────────────────────────────────────────
 *
 * Controls focus, spindle, radial (tracking), and sledge actuators.
 * Interface: bidirectional 3-wire serial (SICL/SIDA/SILD).
 * Protocol: 8 bits MSB-first; GPIO direction toggled at runtime.
 * Status byte: bit 0 = FE (focus error, active low),
 *              bit 1 = TE (tracking error, active low),
 *              bit 2 = SW (sledge switch, active low).
 *
 * ── Subcode Q-channel reader ──────────────────────────────────────────────
 *
 * cd6_read_subcode() is called from subcode_module() once per main-loop
 * iteration.  It checks whether the SCOR falling-edge interrupt has fired
 * (scor_edge == 1) and, if so, shifts 80 bits (10 bytes) out of the
 * CXD2500's serial output via QDA/QCL into Q_buffer.
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Initialisation
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Configure all GPIO pins and leave them in their safe default state.
 *
 * Sets up UCL/UDAT/ULAT (CXD2500), SICL/SIDA/SILD (DSIC2), QDA/QCL/SCOR
 * (subcode), HF/DOOR (sense), and the COMMO bus pins.  Also initialises
 * the Q_buffer to zero and sets audio_cntrl to its startup value.
 */
void driver_init(void);

/**
 * @brief  Post-reset settling delay for the CXD2545Q.
 *
 * The CXD2545Q XRST (pin 81) is active-LOW and is driven by the board's
 * power-on reset circuit.  This function waits for the IC to exit reset
 * and become ready for register writes, then idles all serial buses.
 * Called once from player_init() before servo_init() and cd6_init().
 */
void reset_cxd2545q(void);

/* ─────────────────────────────────────────────────────────────────────────
 * CXD2545Q write interface
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Send a raw 8-bit command byte to the CXD2545Q.
 *
 * Shifts 8 bits LSB-first on UCL/UDAT, then pulses ULAT to latch.
 * No dummy clock preamble — the CXD2545Q CPU interface requires none.
 *
 * @param  data  Command byte to send (e.g. 0xE0 for CLV STOP, 0xA2 for mute).
 */
void cxd2500_wr(uint8_t data);

/**
 * @brief  Write a high-level motor/audio mode to the CXD2545Q.
 *
 * Maps symbolic constants (MOT_OFF_ACTIVE, MOT_STRTM1_ACTIVE, MUTE,
 * FULL_SCALE, etc.) to the actual CXD2545Q command bytes and calls
 * cxd2500_wr() as appropriate.
 *
 * @param  mode  One of the MOT_* / DAC_* / MUTE / FULL_SCALE / ATTENUATE
 *               / SPEED_CONTROL_* constants from serv_def.h.
 */
void cd6_wr(uint8_t mode);

/* ─────────────────────────────────────────────────────────────────────────
 * DSIC2 servo IC
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Shift 8 bits MSB-first into the DSIC2 and strobe the latch.
 *
 * Sets SIDA to output, clocks 8 bits on SICL, then pulses SILD.
 *
 * @param  data  Byte to write (command or payload byte).
 */
void wr_dsic2(uint8_t data);

/**
 * @brief  Read 8 bits MSB-first from the DSIC2.
 *
 * Sets SIDA to input, clocks 8 bits on SICL.
 * Returns the DSIC2 status byte; bit interpretation:
 *   bit 0 — FE: focus error (0 = in focus)
 *   bit 1 — TE: tracking error (0 = on track)
 *   bit 2 — SW: sledge at home switch (0 = closed / at inner limit)
 *
 * @return DSIC2 status byte.
 */
uint8_t rd_dsic2(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Subcode Q-channel reader
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * 10-byte Q-channel subcode buffer populated by cd6_read_subcode().
 *
 * Layout (after BCD→hex conversion by subcode_module()):
 *   [0] CONAD  — control nibble [7:4], address nibble [3:0]
 *   [1] TNO    — track number (0=lead-in, 0xAA=lead-out)
 *   [2] INDEX  — index within track (or special 0xA0/A1/A2 TOC entries)
 *   [3] RMIN   — relative minutes
 *   [4] RSEC   — relative seconds
 *   [5] RFRM   — relative frames
 *   [6] ZERO   — reserved
 *   [7] AMIN   — absolute minutes
 *   [8] ASEC   — absolute seconds
 *   [9] AFRM   — absolute frames
 */
extern uint8_t Q_buffer[10];

/**
 * @brief  Attempt to read one Q-channel subcode frame from the CXD2500.
 *
 * Returns 1 only if the SCOR edge flag (scor_edge) was set AND the QDA
 * line was asserted at the start of the read.  Shifts 80 bits (10 bytes)
 * MSB-first from QDA using QCL as the clock.  Clears scor_edge on entry.
 *
 * @return 1 if a complete frame was captured; 0 otherwise.
 */
int cd6_read_subcode(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Analogue / sense inputs
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Debounced disc presence check.
 *
 * Samples PIN_HF_DET five times; returns non-zero only if all five samples
 * are high (active-high logic, HF signal present = disc detected).
 * The 5-sample requirement suppresses glitches during disc spin-up.
 *
 * @return Non-zero if disc is present; 0 if no disc.
 */
int hf_present(void);

/**
 * @brief  Check whether the drive door (lid) is closed.
 *
 * Reads PIN_DOOR directly (no debounce — mechanical door state is stable).
 *
 * @return Non-zero if door is closed; 0 if open.
 */
int door_closed(void);

/* ─────────────────────────────────────────────────────────────────────────
 * SCOR counter helpers
 *
 * scor_counter is decremented by the SCOR GPIO ISR on every frame edge
 * (75 Hz).  The servo module uses it as a frame-accurate seek timer:
 * init_scor_counter(N) waits for N frames before subcode_module() reads
 * the Q-channel address to check seek convergence.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Load the SCOR down-counter with an initial frame count.
 *
 * The counter will reach zero after @p count SCOR edges (CD frames).
 * @param  count  Number of frames to wait.
 */
void init_scor_counter(uint8_t count);

/**
 * @brief  Test whether the SCOR counter has reached zero.
 * @return Non-zero if counter == 0 (target frame count reached).
 */
int zero_scor_counter(void);

/**
 * @brief  Increment the SCOR counter.
 *
 * Used during multi-jump seeks to extend the wait window if the laser
 * overshot the target and another jump is needed.
 */
void increment_scor_counter(void);

/* enable_scor_counter() was a no-op retained for the 8051 "EA=1; EX0=1"
 * call site.  The SCOR ISR is now attached unconditionally inside
 * timer_init(); the function and its call site have been removed. */

/* ─────────────────────────────────────────────────────────────────────────
 * CXD2500 status polling
 *
 * The original 8051 firmware read individual status bits from the CXD2500
 * by testing flag registers.  In this implementation status_cd6() returns
 * the value of a simulated flag word, updated by cd6_wr() as side-effects
 * of motor control commands.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Read a CXD2500 status flag.
 *
 * MOT_STRT_1 and MOT_STOP are answered by reading the GFS (Gate Frame Sync)
 * GPIO: GFS HIGH = EFM sync present = motor at CLV speed.
 * SUBCODE_READY is answered by the SCOR ISR flag (scor_edge).
 *
 * @param  status_type  One of SUBCODE_READY / MOT_STRT_1 / MOT_STOP / etc.
 *                      (constants defined in serv_def.h).
 * @return 1 if the flag is asserted, 0 otherwise.
 */
int status_cd6(uint8_t status_type);

/* ─────────────────────────────────────────────────────────────────────────
 * Level-meter helpers
 * ──────────────────────────────────────────────────────────────────────── */

/** Most recent peak audio level, low byte (updated by audio_cxd2500). */
extern uint8_t peak_level_low;

/** Most recent peak audio level, high byte. */
extern uint8_t peak_level_high;

/** Shadow register for the CXD2500 audio control byte. */
extern uint8_t audio_cntrl;

/**
 * @brief  Set the peak-level-meter mode bits in the audio shadow register.
 *
 * The level meter mode occupies bits [3:2] of the CXD2500 audio register.
 * This function updates the shadow register and writes it to the IC.
 *
 * @param  mode  Level meter mode value (0–3).
 */
void set_level_meter_mode(uint8_t mode);

/* ─────────────────────────────────────────────────────────────────────────
 * Brake / motor area helpers
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * Current absolute minute position from Q_buffer, in hex.
 * Updated by subcode_module() each frame.  Used by get_area() to index
 * the brake table — outer tracks require longer brake pulses.
 */
extern uint8_t hex_abs_min;

/**
 * @brief  Return the brake table value for the current disc position.
 *
 * Indexes a lookup table using hex_abs_min>>2 (roughly one entry per
 * 4 minutes of disc time) and the current speed mode.  The returned value
 * is the number of radial kick pulses needed to stop the laser at its
 * current radius.  Used by the servo jump algorithm to calibrate deceleration.
 *
 * @return Brake count for the current position.
 */
uint8_t get_area(void);

/* ─────────────────────────────────────────────────────────────────────────
 * Supplementary CXD2545Q / CXA1372Q signal accessors
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Return 1 when the CXA1372Q focus servo is locked.
 *
 * Reads the FOK (Focus OK) GPIO (PIN_FOK).  The CXA1372Q asserts FOK HIGH
 * when the focus loop is closed and the FE (focus-error) magnitude is within
 * the lock window.  This replaces the slower rd_dsic2() serial poll in the
 * hot servo-loop path and also corrects the polarity inversion that existed
 * in the serial-read path.
 *
 * @return 1 = focus locked; 0 = focus lost or not yet acquired.
 */
int fok_locked(void);

/**
 * @brief  Read the CXD2545Q SENS (pin 80) / SCLK (pin 83) serial status.
 *
 * On this PCB revision PIN_SENS is (-1) — the SENS and SCLK lines are not
 * routed to the ATmega4809 header.  sens_read() returns 0 unconditionally.
 * Connecting SENS and SCLK would allow reading per-channel status bits
 * (focus, tracking, CLV lock) from the CXD2545Q's serial output register.
 *
 * @return GPIO level: 1 or 0 (always 0 when PIN_SENS == -1).
 */
int sens_read(void);

#ifdef __cplusplus
}
#endif
