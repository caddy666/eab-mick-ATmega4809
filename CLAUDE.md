# eab-mick — Commodore CD32 Drive Firmware for ATmega4809

A port of the original Philips/Commodore CD32 drive controller firmware
to an **ATmega4809** custom PCB using **PlatformIO** and the **Arduino framework**
(MegaCoreX board package).

---

## Platform

| Item              | Value                                    |
|:------------------|:-----------------------------------------|
| MCU               | ATmega4809 (megaAVR 0-series, 48-pin)    |
| Board package     | MegaCoreX (`platform = atmelmegaavr`)    |
| Framework         | Arduino                                  |
| Clock             | 16 MHz internal oscillator               |
| Upload protocol   | SerialUPDI (USB-UART + 4.7 kΩ resistor)  |

Build and upload:

```bash
pio run -t upload
```

---

## Hardware

### ATmega4809 PCB pin assignments

| Arduino # | Port pin | Signal   | Direction | Description                    |
|:---------:|:--------:|:--------:|:---------:|:-------------------------------|
| D8        | PE3      | LD-ON    | OUT       | Laser diode enable             |
| D14       | PD3      | UCL      | OUT       | CXD2500 serial clock (CLOK)    |
| D21       | PD5      | UDAT     | OUT       | CXD2500 serial data  (DATA)    |
| D20       | PD4      | ULAT     | OUT       | CXD2500 serial latch (XLAT)    |
| D13       | PE2      | QDA      | IN        | Q-channel data from CXD2500    |
| D12       | PE1      | QCL      | OUT       | Q-channel clock to CXD2500     |
| D11       | PE0      | MUTE     | OUT       | Audio mute (active-low)        |
| D15       | PD2      | FOK      | IN        | Focus OK from CXA1372Q         |
| D16       | PD1      | GFS      | IN        | Gate Frame Sync from CXD2500   |
| D17       | PD0      | SCOR     | IN        | Subcode frame clock (75 Hz)    |
| D9        | PB0      | DOOR     | IN        | Door/lid switch                |
| D10       | PB1      | ACTIVE   | OUT       | Status output (LED after boot) |
| D5        | PB2      | PASSIVE  | OUT       | Passive signal                 |
| D19       | PA3      | IF_DIR   | OUT       | COMMO direction control        |
| D18       | PA2      | IF_CLK   | BIDIR     | COMMO clock (we drive it)      |
| D2        | PA0      | IF_DATA  | BIDIR     | COMMO data                     |

### Unconnected signals on this PCB revision

| Symbol      | Reason                                              |
|:------------|:----------------------------------------------------|
| DSIC2 bus   | SICL / SIDA / SILD not brought to header pins       |
| PIN_HF_DET  | No dedicated HF detector pin; `hf_present()` → 1   |
| PIN_SENS    | AREF / PD7 has no standard D-number in MegaCoreX   |

All unconnected pins are defined as `(-1)` in `include/gpio_map.h`.
Every code path that would write to a `(-1)` pin is guarded:

```c
#if PIN_DSIC_CLK >= 0
    // ... DSIC2 code compiled out when not connected
#endif
```

---

## Architecture

```
CD32 Host (Amiga Akiko/Paula)
        │  COMMO 3-wire serial bus
        ▼
  ┌─────────────┐
  │  commo.cpp  │  ← RX/TX state machine, one step per loop()
  └──────┬──────┘
         │
  ┌──────▼──────────┐
  │  dispatcher.c   │  ← packet arbitration, command routing
  └──────┬──────────┘
         │
  ┌──────▼──────────┐
  │  cmd_hndl.c     │  ← opcode validation → player_interface
  └──────┬──────────┘
         │
  ┌──────▼──────────┐
  │  player.c       │  ← process dispatch table, sequence stepper
  └──────┬──────────┘
         │
   ┌─────┴──────────────────────────┐
   │                                │
servo.c / play.c / strtstop.c   driver.cpp
(disc control state machines)   (CXD2500 bit-bang SPI,
                                  Q-channel reader)
```

### Main loop order

```cpp
void loop(void) {
    command_handler();   // 1. push pending command to player
    COMMO_INTERFACE();   // 2. step serial RX/TX state machine
    Dispatcher();        // 3. route packets, queue status updates
    player();            // 4. advance disc-control + background tasks
}
```

---

## Timers

| Timer     | Use                                                     |
|:----------|:--------------------------------------------------------|
| TCB1      | 8 ms software timer tick (CCMP=63999, CLK/2=8 MHz)     |
| SCOR ISR  | attachInterrupt(D17, FALLING) — 75 Hz CD frame boundary |

TCA0 is owned by MegaCoreX for `millis()` / PWM.  TCB1 is free.

The `timers[]` array has 8 entries (one per `timer_id_t`); each is
decremented to zero by the TCB1 ISR every 8 ms.

---

## Key porting decisions

### `delay()` renamed to `blocking_delay()`
Arduino declares `void delay(unsigned long ms)` with C linkage.  Our
firmware had `void delay(void)` (different signature, same name) which
causes a C linker conflict.  Renamed to `blocking_delay()` in `timer.h`
and `timer.cpp`.

### DSIC2 servo IC compiled out
The CXA1372Q DSIC2 servo IC's 3-wire bus (SICL/SIDA/SILD) is not wired
to the ATmega4809 header pins.  Pins are defined as `(-1)`.  The
`#if PIN_DSIC_CLK >= 0` guards in `driver.cpp` silently eliminate all
DSIC2 code paths.  `wr_dsic2()` is a no-op; `rd_dsic2()` returns 0
(no error flags set).

### Pico SDK → Arduino GPIO translation

| Pico SDK                          | Arduino equivalent              |
|:----------------------------------|:--------------------------------|
| `gpio_init(pin)`                  | *(remove — implicit in pinMode)*|
| `gpio_set_dir(pin, GPIO_OUT)`     | `pinMode(pin, OUTPUT)`          |
| `gpio_set_dir(pin, GPIO_IN)`      | `pinMode(pin, INPUT_PULLUP)`    |
| `gpio_pull_up(pin)`               | *(absorbed into INPUT_PULLUP)*  |
| `gpio_put(pin, val)`              | `digitalWrite(pin, val ? HIGH : LOW)` |
| `gpio_get(pin)`                   | `digitalRead(pin)`              |
| `sleep_us(n)`                     | `delayMicroseconds(n)`          |
| `add_repeating_timer_us(-8000,…)` | TCB1 ISR (CCMP=63999)           |
| `gpio_set_irq_enabled_with_callback(PIN_SCOR, FALL, …)` | `attachInterrupt(digitalPinToInterrupt(PIN_SCOR), isr, FALLING)` |

---

## File structure

```
eab-mick/
├── platformio.ini            # PlatformIO build config (ATmega4809, SerialUPDI)
├── CLAUDE.md                 # This file
│
├── include/
│   ├── pins.h                # Canonical ATmega4809 Arduino D-numbers
│   ├── gpio_map.h            # Legacy name aliases + unconnected (-1) pins
│   ├── defs.h                # Master type defs and command opcodes
│   ├── driver.h              # Hardware driver API
│   ├── timer.h               # Software timer API (blocking_delay, timers[])
│   ├── serv_def.h            # Servo states and CXD2500 mode constants
│   ├── dsic2.h               # DSIC2 constants and jump thresholds
│   ├── player.h              # Player module API
│   ├── commo.h               # COMMO serial interface API
│   ├── sts_q_id.h            # Status/Q/ID packet buffer API
│   └── cmd_hndl.h            # Command handler API
│
└── src/
    ├── main.cpp              # Arduino setup() / loop() entry point
    ├── timer.cpp             # TCB1 8 ms tick + SCOR ISR + blocking_delay()
    ├── driver.cpp            # CXD2500 bit-bang, Q-channel reader, GPIO init
    ├── commo.cpp             # COMMO RX/TX state machine
    ├── dispatcher.c          # COMMO packet arbitration + command routing
    ├── cmd_hndl.c            # Opcode → player_interface translation
    ├── sts_q_id.c            # Status/Q/ID packet buffer
    ├── player.c              # Process dispatch table + player tick
    ├── servo.c               # Focus/radial/sledge servo state machine
    ├── subcode.c             # Q-channel subcode reader and BCD conversion
    ├── play.c                # Audio play state machine
    ├── strtstop.c            # Spin-up / spin-down sequences
    ├── shock.c               # Shock/vibration recovery
    ├── service.c             # Service/diagnostic mode
    └── maths.c               # BCD/time arithmetic, track estimation
```

The four `.cpp` files (`main.cpp`, `timer.cpp`, `driver.cpp`, `commo.cpp`)
were ported from Pico SDK to Arduino API.  All other `.c` files contain
pure logic with no platform-specific calls and compile unchanged.

---

## Build notes

- `defs.h` defines `typedef uint8_t byte`.  Arduino also declares
  `typedef uint8_t byte`.  In C++11 (the default for avr-g++), identical
  typedef redeclarations are allowed; no error will result.

- The `extern void Dispatcher(void)` forward declaration in `main.cpp`
  is intentional — `Dispatcher()` is defined in `dispatcher.c` (C linkage)
  and called from `main.cpp` (C++ linkage).  avr-g++ resolves plain C
  function names without mangling, so the linkage is correct.

- Serial (PC5/PC4) is available for debug output if needed.  Add
  `Serial.begin(115200)` in `setup()` and use `Serial.print()` freely;
  it does not conflict with any CD drive signals.
