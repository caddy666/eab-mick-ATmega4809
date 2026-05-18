# Build Requirements — cd32_pico

Firmware for the Commodore CD32 CD drive, targeting the Raspberry Pi Pico 2 (RP2350).

---

## Target hardware

| Item | Value |
|------|-------|
| Board | Raspberry Pi Pico 2 |
| MCU | RP2350 (ARM Cortex-M33, dual-core, 125 MHz default) |
| Pico SDK board ID | `pico2` |
| Platform | `rp2350-arm-s` |

---

## Host toolchain

| Tool | Minimum version | Notes |
|------|-----------------|-------|
| `arm-none-eabi-gcc` | 13.x | Cross-compiler for ARM Cortex-M33. Tested with 13.2.1. |
| `arm-none-eabi-objcopy` | (ships with gcc) | Required by Pico SDK to produce `.uf2` / `.bin` / `.hex` outputs. |
| CMake | 3.13 | Version required by `CMakeLists.txt`; tested with 3.28.3. |
| Ninja | any | Recommended generator (faster than `make`). GNU Make also works. |
| Python 3 | 3.6+ | Used by Pico SDK CMake scripts for UF2 generation. Tested with 3.12.3. |

On Debian/Ubuntu, install with:

```bash
sudo apt install cmake ninja-build python3 gcc-arm-none-eabi libnewlib-arm-none-eabi
```

---

## Pico SDK

| Item | Value |
|------|-------|
| Version | **2.2.0** |
| Tested path | `/home/caddy/.pico-sdk/sdk/2.2.0` |

Pass the path to CMake via `-DPICO_SDK_PATH`:

```bash
cmake -B build -S . -DPICO_SDK_PATH=/path/to/pico-sdk
```

Alternatively, set the environment variable `PICO_SDK_PATH` before calling CMake.

The SDK can be installed via the [Pico SDK installer](https://github.com/raspberrypi/pico-sdk)
or cloned manually:

```bash
git clone --branch 2.2.0 --recurse-submodules \
    https://github.com/raspberrypi/pico-sdk.git
```

---

## C/C++ language standards

| Language | Standard |
|----------|----------|
| C | C11 (`-std=c11`) |
| C++ | C++17 (`-std=c++17`) |

---

## SDK libraries used

These are linked automatically via `target_link_libraries` in `CMakeLists.txt`:

| Library | Purpose |
|---------|---------|
| `pico_stdlib` | GPIO, time, sleep, USB/UART stdio |
| `hardware_gpio` | GPIO interrupt and direction control |
| `hardware_timer` | Repeating timer (`add_repeating_timer_us`) |
| `hardware_irq` | IRQ priority and enable/disable |
| `hardware_pio` | PIO state machine (reserved for future use) |
| `pico_multicore` | Multicore launch (reserved for future use) |

---

## Serial output

USB serial is enabled; UART serial is disabled:

```cmake
pico_enable_stdio_usb(cd32_pico 1)
pico_enable_stdio_uart(cd32_pico 0)
```

A USB CDC serial monitor (e.g. `minicom`, `screen`, `PuTTY`) can be used for
debug output once the firmware is running.

---

## Building

```bash
# 1. Configure (first time only, or after CMakeLists.txt changes)
cmake -B build -S . -DPICO_SDK_PATH=/path/to/pico-sdk

# 2. Compile
cmake --build build

# or with Ninja explicitly:
cmake -B build -S . -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
ninja -C build
```

On a successful build the following artefacts are produced in `build/`:

| File | Use |
|------|-----|
| `cd32_pico.uf2` | Drag-and-drop flash via USB bootloader (recommended) |
| `cd32_pico.elf` | ELF with debug info, for use with OpenOCD / GDB |
| `cd32_pico.bin` | Raw binary |
| `cd32_pico.hex` | Intel HEX |

---

## Flashing

Hold the **BOOTSEL** button on the Pico 2 while plugging in USB. The board
mounts as a mass-storage device. Copy `cd32_pico.uf2` onto it; it will
reboot automatically.

For SWD debugging, connect a Pico Probe (or second Pico running picoprobe) to
the SWD pins and use OpenOCD with the `cmsis-dap` interface.
