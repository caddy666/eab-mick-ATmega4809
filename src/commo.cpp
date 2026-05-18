/**
 * @file  commo.cpp
 * @brief COMMO serial interface state machine — ATmega4809 Arduino port.
 *
 * Implements the 3-wire bidirectional serial protocol between this firmware
 * and the Amiga CD32 host chipset (Paula / Akiko).
 *
 * ── Physical bus ──────────────────────────────────────────────────────────
 *
 *   PIN_COMMO_CLK  — clock; we are always the clock master (OUTPUT).
 *                    Idle-high via pull-up; driven low/high during each byte.
 *   PIN_COMMO_DATA — data, bidirectional; we switch direction per transfer.
 *   PIN_COMMO_DIR  — direction output: 1 = we transmit, 0 = host transmits.
 *
 * ── Receive protocol (host → drive) ──────────────────────────────────────
 *
 *   Host signals intent by pulling DATA low.  We detect this in IDLE via
 *   commo_get_data() == 0.  We then clock in 8 bits (LSB-first on wire,
 *   reconstructed MSB-first in software).
 *
 * ── Transmit protocol (drive → host) ─────────────────────────────────────
 *
 *   We wait for DATA to be released (== 1) then drive DATA and toggle CLK
 *   for all 8 bits.  After the last bit we set DIR=1 and DATA=1 (idle).
 *
 * ── Checksum ──────────────────────────────────────────────────────────────
 *
 *   One's complement of the arithmetic sum of all data bytes (same as IPv4).
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 */

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "commo.h"
#include "gpio_map.h"

/* =========================================================================
 * Internal state machine
 * ====================================================================== */

typedef enum {
    COMMO_SM_IDLE = 0,
    COMMO_SM_RXD_OPCODE,
    COMMO_SM_RXD_PARM,
    COMMO_SM_RXD_CHECKSUM,
    COMMO_SM_TXD_DATA,
    COMMO_SM_TXD_CHECKSUM,
    COMMO_SM_ERR_SEND
} commo_sm_state_t;

typedef struct {
    commo_sm_state_t state;

    uint8_t cmd_length;
    uint8_t checksum;
    uint8_t byte_counter;
    uint8_t byte_pointer;
    uint8_t rx_status;
    uint8_t last_command;

    uint8_t rx_buffer[12];
    uint8_t tx_buffer[16];
    uint8_t tx_length;

    uint8_t tx_req;
    uint8_t tx_chk_req;
    uint8_t report_cmd;
    uint8_t cmd_buf_free;
} commo_ctx_t;

static commo_ctx_t s_commo;

static const uint8_t command_length_table[16] = {
    1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1
};

/* =========================================================================
 * GPIO helpers
 * ====================================================================== */

static inline void commo_set_dir(int v)
{
    digitalWrite(PIN_COMMO_DIR, v ? HIGH : LOW);
}

static inline void commo_set_data_out(int v)
{
    pinMode(PIN_COMMO_DATA, OUTPUT);
    digitalWrite(PIN_COMMO_DATA, v ? HIGH : LOW);
}

static inline int commo_get_data(void)
{
    pinMode(PIN_COMMO_DATA, INPUT_PULLUP);
    return digitalRead(PIN_COMMO_DATA);
}

static inline void commo_set_clk(int v)
{
    pinMode(PIN_COMMO_CLK, OUTPUT);
    digitalWrite(PIN_COMMO_CLK, v ? HIGH : LOW);
}

/* =========================================================================
 * Low-level byte I/O
 * ====================================================================== */

static uint8_t get_rxd_data(void)
{
    uint8_t a = 0;

    commo_set_clk(0);
    for (int i = 0; i < 8; i++) {
        commo_set_clk(1);
        a >>= 1;
        if (commo_get_data()) a |= 0x80u;
        commo_set_clk(0);
    }
    commo_set_dir(0);   /* byte-level acknowledgement */
    return a;
}

static void transmit_txd(uint8_t a)
{
    for (int i = 0; i < 8; i++) {
        commo_set_data_out(a & 1);
        commo_set_clk(0);
        commo_set_clk(1);
        a >>= 1;
    }
    commo_set_dir(1);
    commo_set_data_out(1);
}

/* =========================================================================
 * State machine — one step per main-loop call
 * ====================================================================== */

static void commo_step(commo_ctx_t *c)
{
    switch (c->state) {

    case COMMO_SM_IDLE:
        if (c->tx_req) {
            c->byte_pointer = 0;
            c->checksum     = 0;
            c->state        = COMMO_SM_TXD_DATA;
        } else if (!commo_get_data()) {
            c->state        = COMMO_SM_RXD_OPCODE;
            c->byte_counter = 0;
            c->checksum     = 0;
        }
        break;

    case COMMO_SM_RXD_OPCODE: {
        uint8_t b = get_rxd_data();
        if (b == 0) {
            c->state        = COMMO_SM_ERR_SEND;
            c->byte_counter = 128;
            break;
        }
        c->rx_buffer[0] = b;
        c->checksum     = b;
        c->byte_counter = 1;
        c->cmd_length   = command_length_table[b & 0x0Fu];
        c->state = (c->cmd_length == 1) ? COMMO_SM_RXD_CHECKSUM
                                        : COMMO_SM_RXD_PARM;
        break;
    }

    case COMMO_SM_RXD_PARM:
        if (commo_get_data()) break;
        {
            uint8_t b = get_rxd_data();
            c->rx_buffer[c->byte_counter++] = b;
            c->checksum += b;
            if (c->byte_counter >= c->cmd_length)
                c->state = COMMO_SM_RXD_CHECKSUM;
        }
        break;

    case COMMO_SM_RXD_CHECKSUM:
        if (commo_get_data()) break;
        {
            uint8_t rx = get_rxd_data();
            if ((uint8_t)~rx == c->checksum) {
                c->rx_status = (c->rx_buffer[0] == c->last_command)
                               ? COMMO_SAME_COMMAND
                               : COMMO_NEW_COMMAND;
                if (c->rx_status == COMMO_NEW_COMMAND)
                    c->last_command = c->rx_buffer[0];
            } else {
                c->rx_status    = COMMO_CMD_ERROR;
                c->last_command = 0;
            }
            c->report_cmd = 1;
            c->checksum   = 0;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    case COMMO_SM_TXD_DATA:
        if (!commo_get_data()) break;
        transmit_txd(c->tx_buffer[c->byte_pointer]);
        c->checksum += c->tx_buffer[c->byte_pointer];
        c->byte_pointer++;
        if (--c->byte_counter == 0) {
            c->tx_req = 0;
            c->state  = c->tx_chk_req ? COMMO_SM_TXD_CHECKSUM
                                      : COMMO_SM_IDLE;
        }
        break;

    case COMMO_SM_TXD_CHECKSUM:
        if (!commo_get_data()) break;
        transmit_txd((uint8_t)~c->checksum);
        c->tx_req     = 0;
        c->tx_chk_req = 0;
        c->checksum   = 0;
        c->state      = COMMO_SM_IDLE;
        break;

    case COMMO_SM_ERR_SEND:
        if (c->byte_counter > 0) {
            c->byte_counter--;
        } else {
            c->rx_status  = COMMO_CMD_ERROR;
            c->report_cmd = 1;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    default:
        c->state = COMMO_SM_IDLE;
        break;
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void COMMO_INIT(void)
{
    memset(&s_commo, 0, sizeof(s_commo));
    s_commo.state = COMMO_SM_IDLE;

    pinMode(PIN_COMMO_DIR, OUTPUT);
    digitalWrite(PIN_COMMO_DIR, HIGH);    /* receive mode at startup */

    /* CLK is driven by us as clock master; start idle-high */
    pinMode(PIN_COMMO_CLK, INPUT_PULLUP);

    /* DATA is bidirectional; start as input (host may drive) */
    pinMode(PIN_COMMO_DATA, INPUT_PULLUP);
}

void COMMO_INTERFACE(void)
{
    commo_step(&s_commo);
}

uint8_t NEW_CMD_RECEIVED(void)
{
    if (!s_commo.report_cmd) return COMMO_NO_COMMAND;
    return s_commo.rx_status;
}

uint8_t GET_BUFFER(uint8_t idx)
{
    if (idx >= (uint8_t)sizeof(s_commo.rx_buffer)) return 0;
    return s_commo.rx_buffer[idx];
}

uint8_t SEND_STRING(uint8_t mode, uint8_t *data, uint8_t length)
{
    if (s_commo.tx_req)                               return COMMO_FALSE;
    if (length > (uint8_t)sizeof(s_commo.tx_buffer))  return COMMO_FALSE;

    memcpy(s_commo.tx_buffer, data, length);
    s_commo.byte_counter = length;
    s_commo.tx_length    = length;
    s_commo.tx_chk_req   = (mode == SEND_STRING_COMPLETE) ? 1u : 0u;
    s_commo.tx_req       = 1;

    return COMMO_TRUE;
}

uint8_t SEND_STRING_READY(void)
{
    if (s_commo.state == COMMO_SM_TXD_DATA ||
        s_commo.state == COMMO_SM_TXD_CHECKSUM)
        return COMMO_BUSY;
    return COMMO_READY_WITHOUT_ERROR;
}

uint8_t FREE_CMD_BUFFER(void)
{
    s_commo.report_cmd   = 0;
    s_commo.cmd_buf_free = 1;
    return COMMO_TRUE;
}
