#pragma once

// ── Left Vertical Header (Main Signalling) ──────────────────────────────────
// Signal  → Arduino#  Port/Pin   Notes
#define PIN_LD_ON    8   // Laser Diode ON          D8  / PE3
#define PIN_UCL     14   // CXD2500 serial clock    D14 / PD3  (CLOK)
#define PIN_UDAT    21   // CXD2500 serial data-in  D21 / PD5  (DATA / AIN[5])
#define PIN_ULAT    20   // CXD2500 serial latch    D20 / PD4  (XLAT / AIN[4])
#define PIN_QDA     13   // Q-channel data (input)  D13 / PE2  (SQSO / SPI SCK)
#define PIN_QCL     12   // Q-channel clock (out)   D12 / PE1  (SQCK / CIPO)
#define PIN_MUTE    11   // Hardware mute output    D11 / PE0  (MUTE / COPI)
#define PIN_FOK     15   // Focus OK (input)        D15 / PD2  (FOK  / AIN[2])
#define PIN_GFS     16   // GFS (input)             D16 / PD1  (GFS  / AIN[1])

// ── Right Vertical Header (Interface & Control) ──────────────────────────────
#define PIN_DOOR     9   // Door switch (input)     D9  / PB0  (active-low ~D9)
#define PIN_ACTIVE  10   // Active signal (output)  D10 / PB1  (active-low ~D10)
#define PIN_PASSIVE  5   // Passive (output)        D5  / PB2  (active-low ~D5)
#define PIN_IF_DIR  19   // IF direction            D19 / PA3/PF3 (SCL)
#define PIN_IF_CLK  18   // IF clock                D18 / PA2/PF2 (SDA)
#define PIN_IF_DATA  2   // IF data                 D2  / PA0
#define PIN_SCOR    17   // Subcode SCOR (input)    D17 / PD0  (SUB_SCOR / AIN[0])

// ── DSIC2 serial bus ─────────────────────────────────────────────────────────
// These three signals are not explicitly labelled on the header — verify against
// the PCB schematic before use.  Current assignments are placeholders.
#define PIN_SICL    (-1) // TODO: DSIC2 serial clock
#define PIN_SIDA    (-1) // TODO: DSIC2 serial data (bidirectional)
#define PIN_SILD    (-1) // TODO: DSIC2 serial latch
