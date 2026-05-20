# First Pass: Bugs, Optimisations, and Improvements

A file-by-file review of the ATmega4809 CD32 drive firmware.
No new features. Severity: **[BUG]** = incorrect/unsafe, **[OPT]** = performance, **[QA]** = quality/correctness/maintainability.

---

## `include/defs.h`

### [QA-1] Header says RP2350 — wrong platform
Line 8: `"Adapted for the RP2350 (Raspberry Pi Pico 2)."` — should reference ATmega4809.

### [QA-2] `READY` and `CD_ERROR_STATE` are bare integer literals
`#define BUSY 0 / READY 1 / CD_ERROR_STATE 2 / PROCESS_READY 3` — no type suffix, no enum.
These are used in `uint8_t` returns everywhere. An enum would let the compiler catch wrong-type comparisons and provide exhaustiveness warnings in switches.

---

## `include/dsic2.h`

### [BUG-1] `LASER_OFF` conflicts with `PRESET` — same value, different role
```c
#define PRESET    0x00   // Software reset
#define LASER_OFF 0x00   // Disable laser
```
Both are `0x00`. In `switch_laser_off()`:
```c
wr_dsic2(PRESET);     // sends 0x00 — software reset
wr_dsic2(LASER_OFF);  // sends 0x00 — effectively another PRESET
```
The second byte should be the data argument for the PRESET command.
Since `LASER_ON = 0x01` (the "laser-on" data byte after PRESET), `LASER_OFF = 0x00` is the "laser-off" data byte — correct electrically. But the name `LASER_OFF` sharing `0x00` with `PRESET` is a dangerous aliasing trap.
**Fix:** Rename `LASER_OFF` to `LASER_OFF_DATA` (or similar) and add a comment that these are data bytes, not command bytes.

### [BUG-2] Long-jump `brake_dist` overflows `int8_t`
`servo.c:jump_servo_state()`:
```c
brake_dist = (int8_t)((int)BRAKE_DIS_MAX / -16);
// = (int8_t)(3000 / -16) = (int8_t)(-187)
```
`-187` is outside `int8_t` range (-128…127). The cast is implementation-defined; avr-g++ wraps to `+69`, a large *positive* value. This is then written to the DSIC2 as the brake distance, which is intended to be a large negative number.
**Fix:**
```c
brake_dist = INT8_MIN;   // clamped maximum brake (-128 in uint8_t = 0x80)
```

### [QA-3] `BRAKE_DIS_MAX` and `BRAKE_2_DIS_MAX` are the same value (3000)
Two constants with distinct names for conceptually different things but identical values. If either changes independently in the future, one may be silently left wrong.

### [QA-4] `UPTO_N2_TIME` comment says "Settling time when checking for 2× speed: 80 ms"
But the way it is used in `servo.c::upto_n2_state()`:
```c
if (servo_timer < UPTO_N2_TIME) {   // fires when timer FALLS BELOW 10
```
No timer is loaded before entering `UPTO_N2` state. The state relies on `servo_timer` counting down from whatever it was last set to (often `SUBCODE_MONITOR_TIMEOUT = 25`). The effective wait is non-deterministic.
**Fix:** Load `servo_timer = UPTO_N2_TIME` in `servo_monitor_state()` before transitioning to `UPTO_N2`.

---

## `include/timer.h`

### [QA-5] Header doc says "Pico hardware repeating_timer" — wrong platform
Opening comment references Pico SDK. Should reference TCB1 on ATmega4809.

### [QA-6] Timer write races explained incorrectly
Comment: "Writes are not atomic vs the ISR, but since all writes happen in the main loop...". On 8-bit AVR, a `uint8_t` write is a single instruction and is inherently atomic. The comment overstates the risk and could mislead.

---

## `include/serv_def.h`

### [QA-7] `#define N 1` — dangerously short macro name
`N` is a single character and will silently substitute in any expression containing the letter `N` as an identifier prefix match. Should be `DEFAULT_DISC_SPEED` or similar.

---

## `src/timer.cpp`

### [BUG-3] `blocking_delay()` undefined behaviour when `delay_byte == 0`
```c
void blocking_delay(void)
{
    do {
        delayMicroseconds(500);
    } while (--delay_byte);
}
```
`do { } while (--delay_byte)` always executes at least once. If `delay_byte == 0` on entry, `--delay_byte` wraps to `255` (uint8_t), and the loop runs 256 iterations = **128 ms unintended stall** during a hardware reset sequence.
All three callers in `reset_dsic2_cd6()` set `delay_byte` immediately before the call, so this is latent but real.
**Fix:**
```c
void blocking_delay(void)
{
    while (delay_byte--) {
        delayMicroseconds(500);
    }
}
```

### [OPT-1] `delay_us_500x()` — unsigned int overflow on AVR
```c
void delay_us_500x(uint8_t n)
{
    delayMicroseconds((unsigned int)n * 500u);
}
```
`unsigned int` is 16-bit on AVR. For `n > 131`, `n * 500` overflows (`132 * 500 = 66000 > 65535`). Not currently called anywhere, but the function is exported via `timer.h` and would silently give wrong delays.
**Fix:** Use `uint32_t` for the product, or replace with a loop.

---

## `src/driver.cpp`

### [QA-8] Function-scoped `extern` declarations hide dependencies
`reset_dsic2_cd6()` and `cd6_read_subcode()` both re-declare `extern volatile uint8_t scor_edge;` inside the function body. `scor_edge` is already in `timer.h`. These internal extern declarations should be removed; include `timer.h` at file scope instead.

### [BUG-4] `init_scor_counter()` off-by-one — fires one frame early
```c
void init_scor_counter(uint8_t count)
{
    scor_counter = count;
    if (scor_counter > 0) scor_counter--;
}
```
Loading `count = 1` sets `scor_counter = 0` immediately, so `zero_scor_counter()` returns true on the next check without waiting for a single SCOR edge.
In `jump_time()`, this is called with `delta.frm` to wait the fractional frame count. A delta of 1 frame results in no wait.
**Fix:** Remove the pre-decrement. The ISR decrements on every SCOR edge; the counter reaches zero after exactly `count` edges.
```c
void init_scor_counter(uint8_t count)
{
    scor_counter = count;
}
```

### [OPT-2] `hf_present()` polls 5 times in a tight loop with no delay
Five consecutive `digitalRead()` calls without any delay samples essentially the same instant. If the intent is debouncing, the samples should be spaced (e.g. across loop iterations). If the intent is noise immunity, there should be a micro-delay between samples.

### [QA-9] `cd6_read_subcode()` reads `scor_edge` then `QDA()` before clearing
```c
if (!scor_edge) return 0;
scor_edge = 0;
if (!QDA()) return 0;
```
If QDA is not asserted, `scor_edge` has already been cleared. The SCOR edge is consumed but the frame is discarded silently. On the next tick, no new frame can be captured until the next SCOR fires (75 Hz). This is likely intentional (QDA asserted is the validity check), but should be commented.

---

## `src/commo.cpp`

### [BUG-5] `COMMO_INIT()` sets clock pin as `INPUT_PULLUP` instead of `OUTPUT`
```c
/* CLK is driven by us as clock master; start idle-high */
pinMode(PIN_COMMO_CLK, INPUT_PULLUP);   // ← WRONG
```
Comment contradicts code. The clock should be `OUTPUT`. As-is, CLK floats pulled-high until the first `commo_set_clk()` call, which fixes it via `pinMode(OUTPUT)` inside the function. This means the first clock edge involves an extra `pinMode()` call and the line may glitch.
**Fix:** Change to `pinMode(PIN_COMMO_CLK, OUTPUT); digitalWrite(PIN_COMMO_CLK, HIGH);`

### [BUG-6] `commo_set_clk()` calls `pinMode()` on every clock toggle
```c
static inline void commo_set_clk(int v)
{
    pinMode(PIN_COMMO_CLK, OUTPUT);     // called every bit!
    digitalWrite(PIN_COMMO_CLK, v ? HIGH : LOW);
}
```
`pinMode()` on ATmega4809 is ~100 ns of register setup per call. CLK is always OUTPUT after init. This wastes 16+ `pinMode()` calls per received byte — approximately 1600 extra cycles per byte.
**Fix:** Remove `pinMode()` from `commo_set_clk()`; set OUTPUT once in `COMMO_INIT()`.

### [BUG-7] `commo_get_data()` calls `pinMode(INPUT_PULLUP)` inside the bit-bang read loop
Same issue as BUG-6 but for the DATA line during receive. `pinMode(INPUT_PULLUP)` is called 8 times per received byte even though the pin is already input. Pin direction changes should only occur at byte boundaries (when switching between TX and RX).
**Fix:** Track current DATA direction; only call `pinMode()` on actual direction changes.

### [BUG-8] `SEND_STRING()` does not reject `length == 0`
```c
if (length > (uint8_t)sizeof(s_commo.tx_buffer)) return COMMO_FALSE;
memcpy(s_commo.tx_buffer, data, length);
s_commo.byte_counter = length;
```
If `length == 0`, `byte_counter = 0`. In `COMMO_SM_TXD_DATA`:
```c
if (--c->byte_counter == 0) {
```
Pre-decrement wraps `0 → 255`, causing 255 spurious bytes to be transmitted from uninitialised buffer.
**Fix:** Add `if (length == 0) return COMMO_FALSE;`

### [QA-10] `cmd_buf_free` field in `commo_ctx_t` is written but never read
Set to `1` in `FREE_CMD_BUFFER()`, but no code in the COMMO state machine checks it. Dead state. Remove or use it to gate incoming packet acceptance.

### [QA-11] Magic number `128` in `COMMO_SM_ERR_SEND` has no explanation
```c
c->byte_counter = 128;
```
No named constant, no comment explaining what 128 represents (128 loop iterations × some timing = error recovery delay). Should be `#define COMMO_ERR_DELAY_TICKS 128`.

---

## `src/dispatcher.c`

### [QA-12] `Request_packet_transmit()` has three identical switch cases
`STATUS_UPDATE`, `Q_READY`, and `ID_READY` all call `SEND_STRING(SEND_STRING_COMPLETE, Get_sts_q_id_ptr(), <LENGTH>)` with the same length constant (all three are 15). The three cases could be collapsed to one:
```c
if (mode != NO_UPDATE) {
    if (SEND_STRING(SEND_STRING_COMPLETE, Get_sts_q_id_ptr(), STATUS_PACKET_LENGTH) == COMMO_TRUE) {
        s_acknowledge_update = 1;
        return COMMO_TRUE;
    }
    return COMMO_FALSE;
}
return COMMO_TRUE;
```

---

## `src/cmd_hndl.c`

### [BUG-9] `command_handler()` detects command completion in the same tick it dispatches
The two blocks in `command_handler()` run sequentially within a single call:
1. Block 1 dispatches a command: sets `a_command = new_cmd`, `s_handler_ready = 0`.
2. Block 2 checks `!s_handler_ready && p_status == READY`.

Since `player()` hasn't run yet (it follows in loop()), `p_status` is still `READY` from the previous command. Block 2 fires immediately: `s_handler_ready = 1` (reset prematurely), `Store_update_status(STATUS_UPDATE)` (premature status notification).

After this, `s_handler_ready == 1` — so Block 2 will **never fire again** for this command. The player completes and sets `p_status = READY`, but `command_handler()` misses it. The host only learns of completion via SAME_COMMAND polling.

**Fix:** Use a separate flag to prevent Block 2 from firing in the same tick as Block 1:
```c
void command_handler(void)
{
    uint8_t just_dispatched = 0;
    if (s_pending_cmd != NO_CMD &&
        player_interface.a_command == IDLE_OPC &&
        player_interface.p_status  == READY)
    {
        player_interface.a_command = s_pending_cmd;
        player_interface.param1    = s_pending_p1;
        player_interface.param2    = s_pending_p2;
        player_interface.param3    = s_pending_p3;
        s_pending_cmd   = NO_CMD;
        s_cmd_accepted  = 1;
        s_handler_ready = 0;
        just_dispatched = 1;
    }

    if (!s_handler_ready && !just_dispatched) {
        if (player_interface.p_status == READY ||
            player_interface.p_status == CD_ERROR_STATE)
        {
            s_handler_ready = 1;
            s_cmd_accepted  = 0;
            Store_update_status(STATUS_UPDATE);
        }
    }
}
```

---

## `src/player.c`

### [BUG-10] Service-mode command received in normal mode sets wrong status
```c
} else if (!service_mode) {
    player_error = ILLEGAL_COMMAND;
    process_id   = ERROR_HANDLING_ID;
}
```
The error-handling sequence (`ERROR_HANDLING_ID`) is designed for disc errors. An out-of-mode command should return `CD_ERROR_STATE` directly (as normal-mode commands in service-mode do). The error handling sequence also calls `start_stop(SS_MOTOR_OFF)`, which stops the disc — incorrect for a simple command-rejected response.
**Fix:** Mirror the symmetrical case:
```c
} else if (!service_mode) {
    player_interface.p_status = CD_ERROR_STATE;
    player_interface.param1   = ILLEGAL_COMMAND;
    process_id = IDLE_OPC;
}
```

### [QA-13] `READ_SUBCODE_OPC` has special-case handling inline in the `READY` arm
```c
case READY:
    if (process_id == (byte)(READ_SUBCODE_OPC + 1u)) {
        player_interface.p_status = READY;
        ...
    } else {
        function_id++;
    }
    break;
```
This is the only opcode with inline special-case logic at the `READY` level. It works because `READ_SUBCODE` has a single step that returns `READY` (not `PROCESS_READY`). The step function should return `PROCESS_READY` to use the standard path, or the dispatch table should be adjusted.

---

## `src/servo.c`

### [BUG-11] `radial_recover_state()` does not set `servo_exec_state = BUSY` on entry
When `SERVO_MONITOR` transitions to `RADIAL_RECOVER` after tracking loss, `servo_exec_state` is left at `READY` (set by the monitor). Any caller polling `get_servo_process_state()` immediately after would see `READY` while the drive is actually performing a recovery. The next jump command issued during this window would proceed before the laser is back on track.
**Fix:** Add `servo_exec_state = BUSY;` at the start of `radial_recover_state()`.

### [BUG-12] `servo_retries` is decremented before the first retry attempt
In both `radial_recover_state()` and `focus_recover_state()`:
```c
servo_retries--;
if (servo_retries == 0) { /* error */ }
```
`servo_retries` is loaded to `MAX_RETRIES = 3` in `start_focus_state()` and `servo_jump()`. The first call to `radial_recover_state()` immediately decrements to 2 before any recovery attempt. Only 2 actual recovery attempts are made, not 3.
**Fix:** Decrement at the END of the state after recording a failed attempt, or initialise to `MAX_RETRIES + 1`.

### [OPT-3] `calc_kick()` calls `get_area()` up to three times
```c
if      (get_area() == 1) offset = 6;
else if (get_area() == 2) offset = ...;
else    offset = ...;
```
`get_area()` contains two comparisons on a global. Cache the result:
```c
uint8_t area = get_area();
if      (area == 1) offset = 6;
else if (area == 2) offset = ...;
```

### [BUG-13] `stop_servo_state()` — `motor_on_speed = (uint8_t)!n1_speed` after cutting motor
```c
cd6_wr(MOT_OFF_ACTIVE);
...
motor_on_speed = (uint8_t)!n1_speed;   /* preserve = 1 at 2×, clear = 0 at 1× */
```
Setting `motor_on_speed = 1` immediately after `MOT_OFF_ACTIVE` is semantically misleading. This is using the flag as a "disc was spinning at 2× when stopped" indicator, which `check_stop_servo_state()` uses to add an extra settle stage. The flag name and its use should be separated (use a distinct `was_at_2x` flag) to prevent future confusion.

### [QA-14] `dsic_in_focus()` in `servo.c` uses GPIO (FOK pin), but `service.c` uses DSIC2 serial read
`servo.c`:
```c
static uint8_t dsic_in_focus(void) { return (uint8_t)fok_locked(); }
```
`service.c`:
```c
static uint8_t dsic_in_focus(void) { return rd_dsic2() & 0x01u ? 0u : 1u; }
```
Two functions with the same name in different files that read physically different signals. If the DSIC2 bus is ever connected (PIN_DSIC_CLK ≥ 0), service.c's focus check reads the FE serial bit while servo.c reads the FOK GPIO — they may give different results. The servo.c comment explains the old rd_dsic2() path had an inverted return; service.c still uses the old path. **When DSIC2 is connected, service.c's `dsic_in_focus()` may be inverted.**
**Fix:** Extract `fok_locked()` into a shared focus accessor used by both modules.

---

## `src/play.c`

### [BUG-14] `subtract_time()` called without verifying `pt >= tmp_time` in `jump_time()`
```c
int nr = calc_tracks(&store.play_times.tmp_time, pt);
if (nr == 0) {
    cd_time_t delta;
    subtract_time(pt, &store.play_times.tmp_time, &delta);  // ← pt may be < tmp_time
```
`calc_tracks()` has ~16-groove resolution (divides by 16 at the end). It returns 0 when the position difference rounds to zero — but the actual positions could be in either order. If `tmp_time > pt` (laser is fractionally past the target), `subtract_time()` computes a negative borrow that wraps `r->min` as an unsigned byte.
**Fix:**
```c
if (nr == 0) {
    cd_time_t delta = {0, 0, 0};
    if (compare_time(pt, &store.play_times.tmp_time) != SMALLER)
        subtract_time(pt, &store.play_times.tmp_time, &delta);
    init_scor_counter(delta.frm);
```

### [BUG-15] `monitor_subcodes()` — pointer truncation to `uint8_t` on 16-bit AVR
```c
player_interface.param1 = (uint8_t)(uintptr_t)&store.play_subcode;
```
On ATmega4809 SRAM pointers are 16-bit. Casting to `uint8_t` discards the high byte. This was valid on the 8051's 8-bit data pointer model. The host (Amiga) presumably uses this value as an opaque key or the status packet contains the data itself — but if any code path dereferences `param1` as a pointer, it will access the wrong address.
**Fix:** Document clearly that `param1` is used as a packet-type tag here, not a real pointer. Consider replacing with a type-tag constant (e.g. `SUBCODE_DATA_TAG`) instead of an address.

### [OPT-4] `execute_play_functions()` uses `goto` to share a label with normal fall-through
The `goto monitor` at lines 1041-1045 jumps into the same block reached by the non-goto path. This can be restructured into a helper function call without changing behavior, improving readability.

### [QA-15] `play_monitor` and `play_command_busy` exported as globals but not used externally
Both are declared without `static`. Neither is referenced in the headers. If they are truly internal, add `static`. If external, add declarations to `player.h`.

---

## `src/strtstop.c`

### [QA-16] Comment says "Pico2" — wrong platform
Line 119: `"On the Pico2 each module has its own copy"` — should say ATmega4809.

### [BUG-16] `get_disk_type()` — `case 0:` falls through to `case 1:` without a `break` comment
```c
case 0:
    start_subcode_reading();
    play_timer = SUBCODE_TIMEOUT_VALUE;
    start_stop_process++;
    /* fall through immediately to case 1 */
case 1:
```
The comment notes the intended fall-through, but avr-g++ will still warn (`-Wimplicit-fallthrough`). Add `__attribute__((fallthrough));` or restructure to avoid the fall-through.

### [QA-17] Same intentional fall-through issue in `case 3:` → `case 4:`
```c
case 3:
    sst.toc_info.counter = 5;
    play_timer = SUBCODE_TIMEOUT_VALUE;
    start_stop_process++;
    /* fall through to case 4 */
case 4:
```

---

## `src/shock.c`

### [OPT-5] `reset_time_pool()` (`memset`) called every tick even when returning early
```c
void shock_recover(void)
{
    if (!shock_recovery_active) return;

    reset_time_pool();      // ← always called

    if (!shock_phase0) {
        if (progress_timer != 0) return;   // early exit after memset
```
`memset(time_pool, 0, 6)` executes every tick shock_recovery_active is set, even when the function returns early because the timer hasn't expired. Move `reset_time_pool()` (or replace with stack-local variables) to inside the blocks where the temporaries are actually used.

### [QA-18] `time_pool` static array is unnecessary
`time_pool[2]` is zeroed at entry and used only within that call. Stack-local `cd_time_t position, window;` with explicit initialisation where needed is simpler and avoids the static allocation.

---

## `src/maths.c`

### [QA-19] `subtract_time()` precondition `a >= b` is undocumented at call sites
The function asserts `a >= b` in its doc comment but none of the callers verify this condition before calling. See BUG-14. A runtime check (compile-time `NDEBUG` guard) or an assert would catch violations during development:
```c
void subtract_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r)
{
    // assert(compare_time(a, b) != SMALLER);
    ...
}
```

### [QA-20] `bcd_to_hex()` is defined twice — once in `maths.c`, once `static` in `subcode.c`
```
maths.c:54    uint8_t bcd_to_hex(uint8_t bcd) { ... }   // public
subcode.c:113 static uint8_t bcd_to_hex(uint8_t bcd) { ... }   // private duplicate
```
And `hex_to_bcd()` is duplicated across `maths.c` (public) and `play.c` (static). Factor into a shared header-inline or ensure only one definition exists.

---

## `src/service.c`

### [QA-21] `start_motor()` timeout path uses `FOCUS_ERROR` instead of `MOTOR_ERROR` for speed wait
Phase 3 (waiting for MOT_STRT_1):
```c
if (servo_timer == 0) { player_error = MOTOR_ERROR; return CD_ERROR_STATE; }
```
Correct. But phase 1 (waiting for focus while starting motor):
```c
if (servo_timer == 0) { player_error = FOCUS_ERROR; return CD_ERROR_STATE; }
```
Also correct. The error cleanup in `execute_service_functions()` tears down the right layers based on `player_error`. No bug, but worth confirming on a connected board since `FOCUS_ERROR` triggers a full focus+motor teardown which is appropriate here.

### [QA-22] `service.c` declares `extern uint8_t dsic_in_focus_pub(void)` but this function doesn't exist
Line 97: `extern uint8_t dsic_in_focus_pub(void);` — there is no `dsic_in_focus_pub()` in `servo.c`. The actual static function is `dsic_in_focus()`. This extern declaration is dead/stale. Remove it (service.c has its own `static dsic_in_focus()`).

---

## `src/subcode.c`

### [QA-23] Function-scoped `extern volatile uint8_t scor_edge` in `start_subcode_reading()`
Same pattern as QA-8 in driver.cpp. Should use the `timer.h` declaration at file scope.

### [QA-24] `is_subcode(ABS_TIME)` — CDR detection logic is inverted in comment
```c
case ABS_TIME:
    if (conad_lo == 0x01) {
        if (tno != 0) return TRUE;
        /* Lead-in (tno=0): only accept if CDR (rmin ≤ 90) */
        return (uint8_t)((rmin > 90u || cd_disc) ? FALSE : TRUE);
    }
```
Comment says "only accept if CDR" but condition `(rmin > 90 || cd_disc) ? FALSE : TRUE` means:
- If `cd_disc == 1` (pressed CD): FALSE — don't accept  
- If `cd_disc == 0` (CDR) AND `rmin <= 90`: TRUE — accept

The logic is correct but the comment is misleading. The check is "reject pressed CDs and CDRs with out-of-range rmin". Rewrite the comment to match.

---

## `src/main.cpp`

### [QA-25] `enable_scor_counter()` is a no-op — dead call site
```c
enable_scor_counter();   /* no-op; SCOR ISR attached in timer_init() */
```
`enable_scor_counter()` in driver.cpp is an empty function. The call and the function should both be removed. The comment in `driver.h` still references the 8051 "EA=1; EX0=1" pattern which is long gone.

---

## Cross-Cutting / Architecture

### [BUG-17] `player_error` is a single global written by multiple modules without coordination
`servo.c`, `play.c`, `service.c`, and `player.c` all write `player_error` directly. If two modules set it in the same tick (possible during a servo error that also triggers a play error), whichever writes last wins. There is no mechanism to prevent clobbering.
**Fix:** Only `player.c` should write `player_error`. All sub-modules should return `CD_ERROR_STATE` and set a module-local error code. `player.c` reads the module-local code and writes `player_error` in one controlled place.

### [OPT-6] `digitalWrite()`/`digitalRead()` in hot paths — consider direct port access
The CXD2500 write (`cxd2500_wr`) and Q-channel read (`cd6_read_subcode`) both use Arduino `digitalWrite`/`digitalRead` in tight bit-bang loops (80+ calls per subcode frame at 75 Hz). On ATmega4809, direct `VPORT` register access (1–2 cycles) is ~10× faster than `digitalWrite` (~20–50 cycles including function call and pin validation).
At 75 Hz × 80 bits × ~40 cycles per read/write = ~240,000 cycles per second on GPIO alone, out of 16,000,000 available. Not critical, but worth noting for margin.

### [QA-26] `extern` variable declarations scattered inside function bodies
`scor_edge`, `scor_counter`, `player_error`, and `hex_abs_min` are declared `extern` inside individual function bodies in several .c files rather than through their respective headers. This hides inter-module dependencies and prevents the compiler from catching type mismatches across translation units. All `extern` declarations should be at file scope and pulled from headers.

---

## Summary Table

| ID | File | Severity | Description |
|----|------|----------|-------------|
| BUG-1 | dsic2.h | HIGH | `LASER_OFF` == `PRESET` == 0x00 — aliasing trap |
| BUG-2 | dsic2.h / servo.c | HIGH | `brake_dist` overflows `int8_t` → +69 instead of -187 |
| BUG-3 | timer.cpp | HIGH | `blocking_delay()` wraps on delay_byte=0 → 128 ms stall |
| BUG-4 | driver.cpp | HIGH | `init_scor_counter(1)` fires immediately — off-by-one |
| BUG-5 | commo.cpp | HIGH | CLK pin set INPUT_PULLUP instead of OUTPUT in COMMO_INIT |
| BUG-6 | commo.cpp | MED | `commo_set_clk()` calls `pinMode()` on every clock edge |
| BUG-7 | commo.cpp | MED | `commo_get_data()` calls `pinMode()` inside bit-bang loop |
| BUG-8 | commo.cpp | MED | `SEND_STRING(length=0)` → byte_counter wraps → 255 spurious bytes |
| BUG-9 | cmd_hndl.c | HIGH | Premature completion detection — command done never reported |
| BUG-10 | player.c | MED | Service cmd in normal mode triggers disc-stop via error handler |
| BUG-11 | servo.c | MED | `radial_recover_state()` doesn't set `servo_exec_state = BUSY` |
| BUG-12 | servo.c | LOW | `servo_retries` decremented before first attempt — one fewer retry |
| BUG-13 | servo.c | LOW | `motor_on_speed = !n1_speed` after motor-off is semantically confusing |
| BUG-14 | play.c | HIGH | `subtract_time(pt, tmp_time)` called when pt may be < tmp_time |
| BUG-15 | play.c | MED | 16-bit SRAM pointer cast to uint8_t `param1` — truncates high byte |
| BUG-16 | strtstop.c | LOW | Implicit fallthrough without annotation (compiler warning) |
| BUG-17 | cross-cut | MED | `player_error` global written by multiple modules, no coordination |
| OPT-1 | timer.cpp | LOW | `delay_us_500x(n>131)` — unsigned int overflow on AVR |
| OPT-2 | driver.cpp | LOW | `hf_present()` — 5 instant samples, no spacing |
| OPT-3 | servo.c | LOW | `calc_kick()` calls `get_area()` up to 3 times |
| OPT-4 | play.c | LOW | `goto` in `execute_play_functions()` — use function call |
| OPT-5 | shock.c | LOW | `memset` every tick even when returning early |
| OPT-6 | cross-cut | LOW | `digitalWrite` in bit-bang hot paths |
| QA-1 | defs.h | LOW | Header says RP2350 — wrong platform |
| QA-2 | defs.h | LOW | State codes as bare `#define` ints — should be enum |
| QA-3 | dsic2.h | LOW | BRAKE_DIS_MAX == BRAKE_2_DIS_MAX (same value, distinct names) |
| QA-4 | dsic2.h | MED | UPTO_N2 timer not set before state entry — non-deterministic wait |
| QA-5 | timer.h | LOW | Doc references Pico hardware — wrong platform |
| QA-6 | timer.h | LOW | Timer atomicity comment overstates risk for uint8_t |
| QA-7 | serv_def.h | LOW | `#define N 1` — dangerously short name |
| QA-8 | driver.cpp | LOW | Function-scoped `extern` declarations for `scor_edge` |
| QA-9 | driver.cpp | LOW | Silent discard of SCOR edge when QDA not asserted |
| QA-10 | commo.cpp | LOW | `cmd_buf_free` field written but never read |
| QA-11 | commo.cpp | LOW | Magic number `128` in ERR_SEND state |
| QA-12 | dispatcher.c | LOW | Three identical switch cases can be collapsed |
| QA-13 | player.c | LOW | `READ_SUBCODE_OPC` needs inline special-case in `READY` arm |
| QA-14 | servo.c | MED | `dsic_in_focus` uses FOK GPIO in servo.c but DSIC2 serial in service.c |
| QA-15 | play.c | LOW | `play_monitor`/`play_command_busy` exported but have no extern in headers |
| QA-16 | strtstop.c | LOW | Comment says "Pico2" — wrong platform |
| QA-17 | strtstop.c | LOW | Second implicit fallthrough without annotation |
| QA-18 | shock.c | LOW | `time_pool` static array unnecessary — use stack locals |
| QA-19 | maths.c | LOW | `subtract_time()` precondition unchecked at call sites |
| QA-20 | maths.c | LOW | `bcd_to_hex()` and `hex_to_bcd()` duplicated across files |
| QA-21 | service.c | LOW | Confirm `FOCUS_ERROR` teardown is correct for motor-start phase |
| QA-22 | service.c | MED | `extern uint8_t dsic_in_focus_pub()` — function doesn't exist |
| QA-23 | subcode.c | LOW | Function-scoped `extern` for `scor_edge` |
| QA-24 | subcode.c | LOW | `ABS_TIME` CDR comment inverted |
| QA-25 | main.cpp | LOW | `enable_scor_counter()` is a dead no-op call |
| QA-26 | cross-cut | LOW | `extern` declarations inside function bodies throughout codebase |
