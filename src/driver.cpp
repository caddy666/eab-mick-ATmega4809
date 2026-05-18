/**
 * @file  driver.cpp
 * @brief Hardware driver — ATmega4809 Arduino port.
 *
 * Bit-bangs the CXD2500BQ (CD6), DSIC2 servo IC, and Q-channel subcode
 * reader.  GPIO calls use the Arduino API (digitalWrite / digitalRead /
 * pinMode / delayMicroseconds).
 *
 * DSIC2 (PIN_DSIC_CLK / DATA / LAT) are not routed on this PCB revision;
 * their pin values are (-1) in gpio_map.h.  Every DSIC2 code path is
 * guarded with #if PIN_DSIC_CLK >= 0 so those paths compile away entirely.
 *
 * Likewise, PIN_HF_DET and PIN_SENS are (-1) on this board; see hf_present()
 * and sens_read() for their safe-default fallbacks.
 */

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "driver.h"
#include "gpio_map.h"
#include "serv_def.h"
#include "timer.h"

/* =========================================================================
 * Shared data (extern declarations in driver.h)
 * ====================================================================== */

uint8_t Q_buffer[10]     = {0};
uint8_t audio_cntrl      = 0;
uint8_t peak_level_low   = 0;
uint8_t peak_level_high  = 0;
uint8_t hex_abs_min      = 0;
uint8_t simulation_timer = 0;

int n1_speed = 1;   /* 1 = single-speed (N=1), 0 = double-speed (N=2) */

/* =========================================================================
 * GPIO helpers
 * ====================================================================== */

/* CXD2500BQ (UCL / UDAT / ULAT) */
static inline void UCL(int v)  { digitalWrite(PIN_CXD_CLK,  v ? HIGH : LOW); }
static inline void UDAT(int v) { digitalWrite(PIN_CXD_DATA, v ? HIGH : LOW); }
static inline void ULAT(int v) { digitalWrite(PIN_CXD_LAT,  v ? HIGH : LOW); }

/* DSIC2 — compiled out when not connected */
#if PIN_DSIC_CLK >= 0
static inline void SICL(int v)     { digitalWrite(PIN_DSIC_CLK,  v ? HIGH : LOW); }
static inline void SILD(int v)     { digitalWrite(PIN_DSIC_LAT,  v ? HIGH : LOW); }
static inline void SIDA_OUT(int v) { digitalWrite(PIN_DSIC_DATA, v ? HIGH : LOW); }
static inline int  SIDA_IN(void)   { return digitalRead(PIN_DSIC_DATA); }
#endif

/* Q-channel */
static inline void QCL(int v) { digitalWrite(PIN_QCL, v ? HIGH : LOW); }
static inline int  QDA(void)  { return digitalRead(PIN_QDA); }

/* Disc / door sense */
#if PIN_HF_DET >= 0
static inline int HF_PRESENT(void)  { return digitalRead(PIN_HF_DET); }
#endif
static inline int DOOR_SWITCH(void) { return digitalRead(PIN_DOOR); }

/* Supplementary CXD2500BQ / CXA1372Q signals */
static inline int  GFS_GET(void)   { return digitalRead(PIN_GFS); }
static inline void MUTE_PUT(int v) { digitalWrite(PIN_MUTE, v ? HIGH : LOW); }

#if PIN_SENS >= 0
static inline int SENS_GET(void) {
    pinMode(PIN_SENS, INPUT_PULLUP);
    return digitalRead(PIN_SENS);
}
#endif

static inline int FOK_GET(void) { return digitalRead(PIN_FOK); }

/* =========================================================================
 * driver_init — configure all GPIO pins
 * ====================================================================== */

void driver_init(void)
{
    /* CXD2500BQ control bus — all three lines are outputs */
    pinMode(PIN_CXD_CLK,  OUTPUT);
    pinMode(PIN_CXD_DATA, OUTPUT);
    pinMode(PIN_CXD_LAT,  OUTPUT);

#if PIN_DSIC_CLK >= 0
    /* DSIC2: clock and latch are always outputs; data starts as output */
    pinMode(PIN_DSIC_CLK,  OUTPUT);
    pinMode(PIN_DSIC_DATA, OUTPUT);
    pinMode(PIN_DSIC_LAT,  OUTPUT);
#endif

    /* Q-channel: we drive QCL; QDA is input from CXD2500 */
    pinMode(PIN_QCL, OUTPUT);
    pinMode(PIN_QDA, INPUT_PULLUP);

#if PIN_HF_DET >= 0
    pinMode(PIN_HF_DET, INPUT_PULLUP);
#endif
    pinMode(PIN_DOOR, INPUT_PULLUP);

    /* SCOR interrupt input — ISR attached later by timer_init() */
    pinMode(PIN_SCOR, INPUT_PULLUP);

    /* Supplementary CXD2500BQ / CXA1372Q signals */
    pinMode(PIN_GFS, INPUT_PULLUP);
    pinMode(PIN_MUTE, OUTPUT);
    digitalWrite(PIN_MUTE, HIGH);   /* idle = unmuted (active-low) */
#if PIN_SENS >= 0
    pinMode(PIN_SENS, INPUT_PULLUP);
#endif
    pinMode(PIN_FOK, INPUT_PULLUP);

    /* LD-ON: hardware laser driver enable — absent in 8051 firmware (laser was
     * enabled via DSIC2 LASER_ON command); present on this PCB as a direct
     * GPIO because the DSIC2 bus is not routed.  Default LOW = laser off. */
    pinMode(PIN_LD_ON, OUTPUT);
    digitalWrite(PIN_LD_ON, LOW);

    /* Status output — low until boot completes */
    pinMode(PIN_LED_STATUS, OUTPUT);
    digitalWrite(PIN_LED_STATUS, LOW);

    /* Leave serial buses in idle (de-asserted) state */
#if PIN_DSIC_CLK >= 0
    SILD(1); SIDA_OUT(1); SICL(1);
#endif
    QCL(1); UDAT(1); UCL(1); ULAT(1);
}

/* =========================================================================
 * reset_dsic2_cd6 — hardware power-on reset sequence
 * ====================================================================== */

void reset_dsic2_cd6(void)
{
    delay_byte = 160; blocking_delay();   /* 160 × 500 µs = 80 ms  */
    delay_byte = 40;  blocking_delay();   /*  40 × 500 µs = 20 ms  */
    delay_byte = 1;   blocking_delay();   /*   1 × 500 µs = 500 µs */
    delay_byte = 20;  blocking_delay();   /*  20 × 500 µs = 10 ms  */

#if PIN_DSIC_CLK >= 0
    SILD(1); SIDA_OUT(1); SICL(1);
#endif
    QCL(1); UDAT(1); UCL(1); ULAT(1);

    extern volatile uint8_t scor_edge;
    scor_edge = 0;   /* discard any SCOR glitches during reset */
}

/* =========================================================================
 * cxd2500_wr — send an 8-bit command to the CXD2500BQ
 *
 * Protocol: 3 dummy clock pulses, then 8 data bits LSB-first, then latch.
 * ====================================================================== */

void cxd2500_wr(uint8_t data)
{
    UDAT(0);
    for (int i = 0; i < 3; i++) { UCL(0); UCL(1); }

    for (int i = 0; i < 8; i++) {
        UCL(0);
        UDAT(data & 1);
        UCL(1);
        data >>= 1;
    }

    ULAT(0); ULAT(1);
    UDAT(1);
}

/* =========================================================================
 * audio_cxd2500 — send audio control word with opcode 0x0A
 *
 * Circular right-shift by 2 replicates the 8051 RRC RRC pre-rotation.
 * ====================================================================== */

void audio_cxd2500(uint8_t data)
{
    UDAT(0);
    for (int i = 0; i < 3; i++) { UCL(0); UCL(1); }

    data = (uint8_t)((data >> 2) | (data << 6));

    for (int i = 0; i < 6; i++) {
        UCL(0);
        UDAT(data & 1);
        UCL(1);
        data >>= 1;
    }

    uint8_t cmd = 0x0A;
    for (int i = 0; i < 4; i++) {
        UCL(0);
        UDAT(cmd & 1);
        UCL(1);
        cmd >>= 1;
    }

    ULAT(0); ULAT(1);
    UDAT(1);
}

/* =========================================================================
 * wr_dsic2 — shift 8 bits MSB-first into the DSIC2
 * ====================================================================== */

void wr_dsic2(uint8_t data)
{
#if PIN_DSIC_CLK >= 0
    pinMode(PIN_DSIC_DATA, OUTPUT);

    for (int i = 0; i < 8; i++) {
        SICL(0);
        SIDA_OUT((data >> 7) & 1);
        SICL(1);
        data <<= 1;
    }

    SIDA_OUT(1);
    SILD(0); SILD(1);

    delayMicroseconds(150);
#else
    (void)data;   /* DSIC2 not connected on this PCB revision */
#endif
}

/* =========================================================================
 * rd_dsic2 — read 8 bits MSB-first from the DSIC2
 * ====================================================================== */

uint8_t rd_dsic2(void)
{
#if PIN_DSIC_CLK >= 0
    uint8_t value = 0;

    pinMode(PIN_DSIC_DATA, INPUT_PULLUP);

    SICL(0);
    SILD(0);

    for (int i = 0; i < 8; i++) {
        SICL(0);
        value <<= 1;
        value |= (uint8_t)(SIDA_IN() ? 1u : 0u);
        SICL(1);
    }

    SILD(1);
    pinMode(PIN_DSIC_DATA, OUTPUT);

    delayMicroseconds(150);
    return value;
#else
    return 0;   /* DSIC2 not connected; return safe default (no errors flagged) */
#endif
}

/* =========================================================================
 * cd6_read_subcode — capture one 10-byte Q-channel frame
 * ====================================================================== */

int cd6_read_subcode(void)
{
    extern volatile uint8_t scor_edge;

    if (!scor_edge) return 0;
    scor_edge = 0;

    if (!QDA()) return 0;

    peak_level_low  = 0;
    peak_level_high = 0;

    for (int b = 0; b < 10; b++) {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++) {
            QCL(0); QCL(1);
            v = (uint8_t)((v >> 1) | ((uint8_t)QDA() << 7));
        }
        Q_buffer[b] = v;
    }

    return 1;
}

/* =========================================================================
 * hf_present — test whether the HF signal is present (disc detected)
 * ====================================================================== */

int hf_present(void)
{
#if PIN_HF_DET >= 0
    for (int i = 0; i < 5; i++) {
        if (!HF_PRESENT()) return 1;   /* active low: LOW = HF present */
    }
    return 0;
#else
    return 1;   /* PIN_HF_DET not routed — assume disc present */
#endif
}

int door_closed(void)
{
    return DOOR_SWITCH();
}

/* =========================================================================
 * SCOR counter helpers
 * ====================================================================== */

void init_scor_counter(uint8_t count)
{
    extern volatile uint8_t scor_counter;
    scor_counter = count;
    if (scor_counter > 0) scor_counter--;
}

int zero_scor_counter(void)
{
    extern volatile uint8_t scor_counter;
    return scor_counter == 0;
}

void increment_scor_counter(void)
{
    extern volatile uint8_t scor_counter;
    scor_counter++;
}

void enable_scor_counter(void)
{
    /* attachInterrupt() is called inside timer_init(); no-op retained for
     * the original call-site in main.cpp / 8051 "EA=1; EX0=1". */
}

/* =========================================================================
 * status_cd6 — poll a CXD2500BQ status flag
 * ====================================================================== */

int status_cd6(uint8_t status_type)
{
    switch (status_type) {
    case MOTOR_OVERFLOW:
        return 0;
    case MOT_STRT_2:
        return 0;
    case MOT_STRT_1:
        return GFS_GET();        /* GFS HIGH = EFM sync = motor at CLV speed */
    case MOT_STOP:
        return 1 - GFS_GET();   /* GFS LOW  = no EFM sync = motor stopped   */
    case SUBCODE_READY:
        return (int)scor_edge;
    default:
        return 0;
    }
}

/* =========================================================================
 * get_area — radial zone for brake/kick duration selection
 * ====================================================================== */

uint8_t get_area(void)
{
    if (hex_abs_min < 16) return 1;
    if (hex_abs_min > 32) return 3;
    return 2;
}

/* =========================================================================
 * set_level_meter_mode
 * ====================================================================== */

void set_level_meter_mode(uint8_t mode)
{
    audio_cntrl &= 0xF0u;
    switch (mode) {
    case 0: audio_cntrl |= NORMAL_MODE; break;
    case 1: audio_cntrl |= LEVEL_MODE;  break;
    case 2: audio_cntrl |= PEAK_MODE;   break;
    default: break;
    }
}

/* =========================================================================
 * cd6_wr — high-level motor / audio command dispatcher
 * ====================================================================== */

void cd6_wr(uint8_t mode)
{
    switch (mode) {

    case MOT_OFF_ACTIVE:     cxd2500_wr(0xE0); break;
    case MOT_BRM1_ACTIVE:    cxd2500_wr(0xEA); break;
    case MOT_BRM2_ACTIVE:    cxd2500_wr(0xEA); break;

    case MOT_STRTM1_ACTIVE:
        cxd2500_wr(0xE8);
        hex_abs_min = 0;
        break;

    case MOT_STRTM2_ACTIVE:  cxd2500_wr(0xEE); break;
    case MOT_JMPM_ACTIVE:
    case MOT_JMPM1_ACTIVE:   cxd2500_wr(0xEE); break;
    case MOT_PLAYM_ACTIVE:   cxd2500_wr(0xE6); break;

    case SPEED_CONTROL_N1:
        n1_speed = 1;
        cxd2500_wr(0x99);
        break;

    case SPEED_CONTROL_N2:
        n1_speed = 0;
        cxd2500_wr(0x9D);
        break;

    case MOT_GAIN_8CM_N1:
    case MOT_GAIN_12CM_N1:   cxd2500_wr(0xC1); break;
    case MOT_GAIN_8CM_N2:
    case MOT_GAIN_12CM_N2:   cxd2500_wr(0xC6); break;
    case DAC_OUTPUT_MODE:    cxd2500_wr(0x89); break;
    case MOT_OUTPUT_MODE:    cxd2500_wr(0xD0); break;
    case EBU_OUTPUT_MODE:    break;   /* not connected on this revision */

    case MUTE:
        audio_cntrl |= 0x20u;
        audio_cxd2500(audio_cntrl);
        MUTE_PUT(0);    /* active-low hard-mute */
        break;

    case FULL_SCALE:
        if (audio_cntrl & 0x20u) {
            audio_cntrl &= 0xCFu;
            audio_cxd2500(audio_cntrl);
            MUTE_PUT(1);
        }
        break;

    case ATTENUATE:
        audio_cntrl &= 0xCFu;
        audio_cntrl |= 0x10u;
        MUTE_PUT(1);
        audio_cxd2500(audio_cntrl);
        break;

    default:
        cxd2500_wr(mode);
        break;
    }
}

/* =========================================================================
 * Supplementary signal accessors
 * ====================================================================== */

int fok_locked(void) { return FOK_GET(); }

int sens_read(void)
{
#if PIN_SENS >= 0
    return SENS_GET();
#else
    return 0;   /* SENS not routed */
#endif
}
