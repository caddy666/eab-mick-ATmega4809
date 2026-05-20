# Second Pass: Bugs, Optimisations, and Improvements

A file-by-file review of the ATmega4809 CD32 drive firmware against the
**CXD2545Q datasheet** (the target IC on this PCB revision).
No new features. Severity: **[BUG]** = incorrect/unsafe, **[OPT]** = performance, **[QA]** = quality/correctness/maintainability.

Reference document: `docs/cxd2545q.txt`

---

## `include/defs.h`

### [QA-1] File header still says "CD32 Pico firmware"
Line 1–3: Opening comment describes the file as part of a "Pico" firmware port.
After the first-pass rename of the RP2350 references in `timer.h`, this header
is now the only remaining "Pico" mention in the codebase.
**Fix:** Change header to reference ATmega4809 / eab-mick project.

### [QA-2] `int_hl_t.val` uses `int` — non-portable width
```c
typedef struct { int val; } int_hl_t;
```
On 8-bit AVR, `int` is 16 bits. If any code is ever compiled on a 32-bit host
(simulation, unit tests), `sizeof(int_hl_t)` silently doubles. Already
used for signed arithmetic in `servo.c`.
**Fix:** Use `int16_t val` for explicit width.

---

## `include/serv_def.h`

### [QA-3] All CXD-register constants are CXD2500BQ values — wrong IC
Every motor/speed constant in this file (`SPEED_CONTROL_N1 = 0xB3`,
`SPEED_CONTROL_N2 = 0xBB`, `DAC_OUTPUT_MODE = 0x9D`, `MOT_OUTPUT_MODE = 0xE0`,
`MOT_GAIN_12CM_N1 = 0xEA`, `MOT_GAIN_12CM_N2 = 0xEE`, `MOT_STRT_1 = 0xE8`,
`MOT_STRT_2 = 0xE6`, `SUBCODE_READY = 0xC1`, `SUBCODE_NOT_READY = 0xC6`,
`HF_SSKIP_MODE = 0x89`, `HF_CLV_MODE = 0xD0`) are register codes for the
**CXD2500BQ**. The CXD2545Q has a different register map and these values will
misconfigure the IC.

This is an architectural cross-cutting concern. The constants must be updated
once the CXD2545Q register map is confirmed. Each constant is listed here so
the required substitution is unambiguous:

| Constant           | CXD2500BQ value | Role                       |
|:-------------------|:---------------:|:---------------------------|
| SPEED_CONTROL_N1   | 0xB3            | PLL config, 1× CLV speed   |
| SPEED_CONTROL_N2   | 0xBB            | PLL config, 2× CLV speed   |
| DAC_OUTPUT_MODE    | 0x9D            | Select audio DAC output     |
| MOT_OUTPUT_MODE    | 0xE0            | Configure motor drive       |
| MOT_GAIN_12CM_N1   | 0xEA            | Motor gain, 12 cm, 1×       |
| MOT_GAIN_12CM_N2   | 0xEE            | Motor gain, 12 cm, 2×       |
| MOT_STRT_1         | 0xE8            | Motor start phase 1 (GFS)   |
| MOT_STRT_2         | 0xE6            | Motor start phase 2 (GFS)   |
| SUBCODE_READY      | 0xC1            | Subcode output enabled      |
| SUBCODE_NOT_READY  | 0xC6            | Subcode output disabled     |
| HF_SSKIP_MODE      | 0x89            | HF single-skip mode         |
| HF_CLV_MODE        | 0xD0            | HF CLV lock mode            |

**Fix:** Consult CXD2545Q register table and replace all values. Add a comment
in the header noting which IC revision the values target so a future reader
can cross-check.

---

## `src/driver.cpp`

### [BUG-1] (CRITICAL) MUTE pin polarity is inverted
`driver_init()` sets `PIN_MUTE HIGH` labelled "audio unmuted". In the
**CXD2545Q datasheet** (pin 79, MUTE): *"high: mute, low: non-mute."*
The initialisation therefore mutes the output at boot and never releases it.

In `audio_cxd2500()`:
```c
case MUTE:       MUTE_PUT(0);   // intended mute   — actually UNmutes
case FULL_SCALE: MUTE_PUT(1);   // intended unmute — actually mutes
```
Every audio-state transition is inverted.

**Fix:**
```c
// driver_init():
digitalWrite(PIN_MUTE, LOW);   // LOW = non-mute per CXD2545Q pin 79

// audio_cxd2500():
case MUTE:       MUTE_PUT(1);   // HIGH = mute
case FULL_SCALE: MUTE_PUT(0);   // LOW  = non-mute
```

### [BUG-2] (CRITICAL) `cxd2500_wr()` sends 3 dummy clocks — CXD2500BQ-only protocol
```c
void cxd2500_wr(uint8_t data)
{
    UCL(0); UCL(1);   // dummy clock 1
    UCL(0); UCL(1);   // dummy clock 2
    UCL(0); UCL(1);   // dummy clock 3
    for (int i = 0; i < 8; i++) { ... }
}
```
The 3 leading dummy clocks are part of the **CXD2500BQ** command framing
(required to enter write mode on that IC). The **CXD2545Q** 3-wire serial
interface (CLOK/DATA/XLAT, pins 88/86/87) does not use this preamble — the
datasheet shows a simple MSB/LSB data stream clocked directly into CLOK with
no preamble. Sending 3 extra clocks will corrupt the bit alignment of every
register write.

**Fix:** Remove the three `UCL(0); UCL(1);` dummy-clock pairs before the data loop.

### [BUG-3] (CRITICAL) `audio_cxd2500()` sends a 10-bit CXD2500BQ-specific audio control frame
The `cxd2500_wr_10bit()` function (or equivalent) in `audio_cxd2500()` sends
two extra bits (channel-select + mode) that are specific to the CXD2500BQ audio
volume/attenuation register format. The CXD2545Q audio control registers have
a different format. Sending the wrong bit-width frame corrupts the register
address decode on the CXD2545Q.

**Fix:** Replace `audio_cxd2500()` with a CXD2545Q-compatible register write
once the CXD2545Q audio volume register format is confirmed from the datasheet.
Until then, stub the function as a no-op to prevent sending malformed data.

### [BUG-4] SPI clock pulse width may violate CXD2545Q 750 ns minimum
Each `UCL(0); UCL(1);` expands to two `digitalWrite()` calls. On ATmega4809
at 16 MHz, each `digitalWrite()` takes roughly 2–4 µs (AVR Arduino overhead).
The resulting clock pulse is ~2–4 µs wide — comfortably above the 750 ns
minimum stated in the CXD2545Q AC timing table.

However, the **latch pulse** `ULAT(0); ULAT(1);` is also two `digitalWrite()`
calls with no explicit hold between them. This is similarly safe, but only
because of `digitalWrite()`'s overhead. If `ULAT` is ever macro-replaced with
direct PORT writes (for speed), a single-cycle pulse would violate the
750 ns minimum.

**Fix (preventive):** Add a `__builtin_avr_nop()` or `delayMicroseconds(1)`
between `ULAT(0)` and `ULAT(1)` with a comment referencing the 750 ns
minimum from the CXD2545Q datasheet so the constraint is visible if the
macro implementation changes.

### [QA-4] `simulation_timer` is declared and set but never used
```c
static uint8_t simulation_timer = 0;   // declared
...
simulation_timer = 100;                // set in one code path
```
`simulation_timer` is never decremented, never read, and has no effect.
It is dead code; the name suggests a leftover from a simulation/debugging
scaffolding that was never wired up.
**Fix:** Remove the declaration and the assignment.

### [QA-5] `reset_dsic2_cd6()` function name is stale
The function resets the CXD2545Q via `PIN_RESET` (XRST, pin 81 on CXD2545Q).
Its name references "dsic2" (the old separate servo IC, which is not
connected). The DSIC2 has its own independent reset path that does not exist
on this PCB.
**Fix:** Rename to `reset_cxd2545q()` and update the call sites in
`player.c` / `player_init()`.

### [QA-6] `hf_present()` hardcoded to `return 1` — HF_DETECTOR_ERROR can never fire
`PIN_HF_DET = (-1)` (no dedicated HF detector pin on this PCB). The
`hf_present()` function therefore always returns 1, and the
`HF_DETECTOR_ERROR` path in `check_ttm_state()` can never be reached.
This is intentional but undocumented — a future reader may wonder why the
error path is dead code.
**Fix:** Add a comment in `hf_present()` noting that the pin is not connected
on this PCB revision and that `HF_DETECTOR_ERROR` is permanently suppressed.

---

## `src/servo.c`

### [BUG-5] (CRITICAL) `wait_subcode_state()` check is inverted — on-track confirmed immediately
```c
// Current code:
if (!status_cd6(SUBCODE_READY)) {
    servo_state = ON_TRACK_MODE;
}
```
`start_subcode_reading()` clears `scor_edge = 0` before this state is entered.
`status_cd6(SUBCODE_READY)` calls `cd6_wr(SUBCODE_READY)` and reads back
`scor_edge` — which is 0 (cleared). The `!0` is `TRUE`, so the firmware
transitions to `ON_TRACK_MODE` immediately on the first loop iteration,
before any subcode frame has been received.

The intent is the opposite: wait until `scor_edge != 0` (a real SCOR pulse
has arrived), then confirm lock. The condition should be:
```c
if (status_cd6(SUBCODE_READY)) {   // scor_edge != 0 → frame received
    servo_state = ON_TRACK_MODE;
}
```
**Fix:** Remove the `!` from the condition.

### [BUG-6] `downto_n1_state()` — `MOT_STRT_2` always returns 0, wait skipped entirely
```c
if (!status_cd6(MOT_STRT_2) || servo_timer == 0) {
    servo_state = WAIT_SUBCODE;
}
```
`status_cd6(MOT_STRT_2)` calls `cd6_wr(MOT_STRT_2)` and reads `scor_edge`.
`MOT_STRT_2 = 0xE6` is a CXD2500BQ-specific register code that the CXD2545Q
does not recognise. `scor_edge` is either cleared by `start_subcode_reading()`
or unrelated to motor phase. In practice `status_cd6(MOT_STRT_2)` returns 0,
so `!0 = TRUE` and the state immediately advances without waiting for the
brake timer. This makes the spindle speed transition non-deterministic.
**Fix:** Remove the `status_cd6(MOT_STRT_2)` term entirely; use only the
timer guard:
```c
if (servo_timer == 0) {
    servo_state = WAIT_SUBCODE;
}
```

### [BUG-7] `active_brake_ok()` guards on `!disc_size` — blocks braking on 8 cm discs
```c
if (!disc_size) return 0;   // disc_size: DISC_8CM=0, DISC_12CM=1
```
`DISC_8CM = 0` is falsy, so an 8 cm disc always returns 0 from
`active_brake_ok()`, disabling active braking entirely for 8 cm media.
The intent is to skip braking when the disc size is *unknown* (not yet
determined from the TOC). There is a separate `disc_size_known` flag for
this purpose.
**Fix:**
```c
if (!disc_size_known) return 0;
```

### [QA-7] `dsic_on_track()` is a permanently misleading stub
```c
uint8_t dsic_on_track(void) {
    return (rd_dsic2() & 0x02u) ? 0u : 1u;
}
```
`rd_dsic2()` always returns 0 (DSIC2 not connected, compiled out).
`(0 & 0x02) = 0`, so `dsic_on_track()` always returns 1 — always "on track".
Any caller that relies on this to detect tracking loss will never detect it.

**Fix:** Add a comment documenting that this function always returns 1 on this
PCB revision because the DSIC2 bus is not connected. Callers in `servo.c`
should be audited to confirm that the "always on-track" assumption is safe.

### [QA-8] All `wr_dsic2()` call-sites are architectural dead code — document clearly
`init_dsic2_state()`, `sledge_in()`, `sledge_out()`, `sledge_off()`,
`rad_start()`, `rad_hold()`, `jump_short()`, `jump_long()` all consist
exclusively of `wr_dsic2(...)` calls. Because `PIN_DSIC_CLK = (-1)`, every
`wr_dsic2()` is a compile-time no-op (`#if PIN_DSIC_CLK >= 0` guard in
`driver.cpp`). The mechanical servo control layer is entirely inoperative.

This is an expected consequence of the CXD2545Q's integrated servo replacing
the external DSIC2/CXA1372 IC. However, it is not documented at the
call-site level — a future developer reading `jump_short()` will not know
it does nothing.

**Fix:** Add a comment block at the top of the DSIC2 servo section in `servo.c`
explaining that all `wr_dsic2()` calls are no-ops on this PCB because the
CXD2545Q integrates servo control internally and the DSIC2 bus is not wired.

---

## `src/service.c`

### [QA-9] Dead extern `dsic_on_track_pub` still declared
```c
extern uint8_t dsic_on_track_pub(void);   // line ~97
```
`dsic_on_track_pub()` does not exist in `servo.c` or any other translation
unit. The first-pass fix (QA-22) removed `dsic_in_focus_pub` but missed this
companion declaration. It causes no linker error in C only because the
function is never called from this file.
**Fix:** Remove the dead `extern` declaration.

---

## `src/subcode.c`

### [QA-10] `cd6_init()` register sequence targets CXD2500BQ — wrong IC
```c
void cd6_init(void) {
    cd6_wr(SPEED_CONTROL_N1);   // 0xB3
    cd6_wr(DAC_OUTPUT_MODE);    // 0x9D
    cd6_wr(MOT_OUTPUT_MODE);    // 0xE0
    cd6_wr(MOT_GAIN_12CM_N1);   // 0xEA
}
```
All four register codes are CXD2500BQ-specific (see QA-3 in `serv_def.h`).
Sending them to the CXD2545Q will write to unintended registers (or be
ignored), leaving the IC in an uninitialised state.
**Fix:** Replace with the CXD2545Q startup register sequence once the
register map is confirmed. Until then, either no-op the function body with
a comment or send only the CXD2545Q reset/mode byte.

---

## `src/strtstop.c`

### [QA-11] `XRST` / `PIN_RESET` polarity assumption should be documented
`reset_dsic2_cd6()` (called from `player_init()`) drives `PIN_RESET` LOW to
assert reset, then HIGH to release. The CXD2545Q XRST (pin 81) is
*"reset when low"* — this matches. However the comment in `driver.cpp` refers
to the signal as `RESET_N` or uses the DSIC2 naming, making the polarity
contract non-obvious.
**Fix:** Add a one-line comment referencing CXD2545Q pin 81 and its active-low
polarity so the function body is self-documenting.

---

## `src/play.c`

### [QA-12] `status_cd6()` call in `play_monitor_state()` passes CXD2500BQ register code
```c
if (status_cd6(SUBCODE_READY)) { ... }
```
`SUBCODE_READY = 0xC1` is a CXD2500BQ register value. Under the CXD2545Q,
`cd6_wr(0xC1)` writes an unintended register. The downstream `scor_edge`
test may coincidentally work (SCOR is wired to the interrupt regardless of
what is written), but the write is incorrect.
This is a consequence of QA-3 (wrong register map) and is separately
listed here because the function is safety-critical for audio output timing.
**Fix:** Address as part of the `serv_def.h` register-map update (QA-3).

---

## Cross-cutting: CXD2545Q hardware interface not yet exploited

These are **not** feature requests — they are existing firmware code paths
that silently degrade or error because the CXD2545Q provides a capability
that the firmware does not yet wire up correctly.

### [QA-13] CXD2545Q LOCK pin (pin 98) not connected, GFS used as proxy instead
The CXD2545Q provides a `LOCK` output (pin 98) that is a debounced,
hardware-filtered version of GFS: it goes high only after GFS has been
consistently high, and goes low after 8 consecutive low samples at 460 Hz.
The firmware currently uses raw `GFS` edge-counting (`MOT_STRT_1` logic in
`servo.c`) to detect CLV lock, which is noise-sensitive.
`LOCK` is not listed in `pins.h` / `gpio_map.h` — it is not connected on
this PCB revision.
**Fix (documentation):** Add `PIN_LOCK (-1)` to `gpio_map.h` with a comment
explaining that routing LOCK to an input pin would improve CLV-lock detection
robustness, and mark it as a recommended PCB-revision wiring change.

### [QA-14] CXD2545Q SENS pin (pin 80) not read — status register inaccessible
The CXD2545Q exposes a serial status output on `SENS` (pin 80), clocked out
via `SCLK` (pin 83). This is the primary way to read back IC status
(focus error magnitude, CLV error, etc.) on the CXD2545Q — unlike the
CXD2500BQ which returned status via the same `scor_edge` / `GFS` feedback
loop. `PIN_SENS = (-1)` in `gpio_map.h`; the read path is permanently
disabled.
**Fix (documentation):** Add a comment to `rd_cd6()` / `status_cd6()` in
`driver.cpp` explaining that on the CXD2545Q, status is read via SENS/SCLK
and the current implementation (reading `scor_edge` as a status proxy) is
an approximation that may not correctly reflect all IC states.

---

## Summary

| #      | File            | Severity | Short description                                    |
|:-------|:----------------|:---------|:-----------------------------------------------------|
| BUG-1  | driver.cpp      | CRITICAL | MUTE polarity inverted (CXD2545Q HIGH=mute)          |
| BUG-2  | driver.cpp      | CRITICAL | 3 dummy clocks in `cxd2500_wr()` — CXD2500BQ only   |
| BUG-3  | driver.cpp      | CRITICAL | 10-bit audio frame format — CXD2500BQ only           |
| BUG-4  | driver.cpp      | TIMING   | ULAT pulse width — add nop guard for future safety   |
| BUG-5  | servo.c         | CRITICAL | `wait_subcode_state()` condition inverted            |
| BUG-6  | servo.c         | HIGH     | `downto_n1_state()` timer guard bypassed immediately |
| BUG-7  | servo.c         | HIGH     | `active_brake_ok()` blocks 8 cm disc braking         |
| QA-1   | defs.h          | LOW      | Header still says "Pico firmware"                    |
| QA-2   | defs.h          | LOW      | `int_hl_t.val` uses `int` instead of `int16_t`       |
| QA-3   | serv_def.h      | CRITICAL | All motor constants are CXD2500BQ register values    |
| QA-4   | driver.cpp      | LOW      | `simulation_timer` dead variable — remove            |
| QA-5   | driver.cpp      | LOW      | `reset_dsic2_cd6()` name refers to absent IC         |
| QA-6   | driver.cpp      | LOW      | `hf_present()` stub not documented                  |
| QA-7   | servo.c         | MEDIUM   | `dsic_on_track()` always returns 1 — undocumented    |
| QA-8   | servo.c         | MEDIUM   | DSIC2 servo call-sites are silent no-ops             |
| QA-9   | service.c       | LOW      | Dead `extern dsic_on_track_pub` declaration          |
| QA-10  | subcode.c       | CRITICAL | `cd6_init()` sends CXD2500BQ startup sequence        |
| QA-11  | strtstop.c      | LOW      | XRST polarity contract undocumented                  |
| QA-12  | play.c          | MEDIUM   | `status_cd6(SUBCODE_READY)` writes wrong register    |
| QA-13  | gpio_map.h      | INFO     | LOCK pin not mapped — document for future PCB rev    |
| QA-14  | driver.cpp      | INFO     | SENS/SCLK status read path not connected             |

**Priority order for fixes:**
1. BUG-1 (MUTE polarity) — causes silent audio at all times
2. QA-3 + QA-10 (register map + init sequence) — IC will not initialise correctly
3. BUG-2 (dummy clocks) — all register writes misaligned
4. BUG-3 (10-bit audio frame) — audio control writes corrupt IC state
5. BUG-5 (wait_subcode inverted) — on-track confirmed without lock
6. BUG-6 + BUG-7 (servo timer/brake guards) — non-deterministic spin behaviour
7. Remaining QA items — documentation and cleanup
