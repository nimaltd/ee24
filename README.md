# 💾 ee24

[![CI](https://github.com/nimaltd/ee24/actions/workflows/ci.yml/badge.svg)](https://github.com/nimaltd/ee24/actions/workflows/ci.yml)
[![Stars](https://img.shields.io/github/stars/nimaltd/ee24?style=social)](https://github.com/nimaltd/ee24)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE.md)

A driver for 24xx I2C EEPROMs, from 24C01 to 24C512, written in C for the STM32 HAL.

Read or write any number of bytes at any address, and the library deals with the rest: pages, blocks, the chip's write time, and the limits of the HAL. It works on any STM32 family, bare metal or with FreeRTOS or ThreadX.

---

## ✨ What you get

- Any length at any address. Page boundaries, the 256 byte blocks of the small
  chips and the 64 KB HAL limit are all handled for you
- Every size from 24C01 to 24C512, chosen per chip, so two different chips can
  share one project
- Writes wait only as long as the chip needs, by asking it every millisecond,
  instead of a fixed 10 ms a page
- An optional write protect pin, lifted only while a write runs
- FreeRTOS, through CMSIS-RTOS v1 or v2, and ThreadX, by way of
  [osal](https://github.com/nimaltd/osal): a mutex per chip, and waits that let your
  other threads run
- A read or write past the end of the chip is refused, never wrapped round to
  overwrite the start
- Unit tested on every commit, against a model of the chip

---

## 🔌 Supported chips

Any 24xx serial EEPROM with an I2C bus: Microchip 24LC and 24AA, Atmel/Microchip AT24C, ST M24, onsemi CAT24, and the many compatible parts. Tell `ee24_init()` the number in the part name, in kilobits:

| Chip | `size_kbit` | Bytes |
|---|---|---|
| 24C01 | `1` | 128 |
| 24C02 | `2` | 256 |
| 24C04 | `4` | 512 |
| 24C08 | `8` | 1 KB |
| 24C16 | `16` | 2 KB |
| 24C32 | `32` | 4 KB |
| 24C64 | `64` | 8 KB |
| 24C128 | `128` | 16 KB |
| 24C256 | `256` | 32 KB |
| 24C512 | `512` | 64 KB |

1 Mbit parts such as the 24LC1025 and M24M01 are not supported: each maker puts the top address bit in a different place.

---

## 📁 Layout

```
src/    ee24.h, ee24.c
test/   host unit tests, run on a PC
```

Installed into a project, the code keeps its `src/` folder, with the README,
changelog and licence files around it. There is no `test/`: the section below
about the tests refers to this repository, not to an installed copy.

---

## ⚙️ Installing it

[stm32-installer](https://github.com/nimaltd/stm32-installer) copies the library into your project and adds it to your CMake, STM32CubeIDE, Keil, IAR or Makefile project for you. Your project file is backed up first. It also checks that I2C is enabled in your CubeMX project, and says so if it is not.

Install it once per machine:

```bash
pip install stm32-installer
```

Then, from the root of your STM32 project:

```bash
stm32-installer nimaltd/ee24
```

ee24 waits and locks through [osal](https://github.com/nimaltd/osal), so the installer puts osal in first, and asks for its folder like any library. If another library already brought osal into your project, that copy is kept and nothing is asked.

### From a downloaded zip

Downloaded this repository with **Code**, **Download ZIP**? Give the installer the zip in place of `nimaltd/ee24`, with no need to unpack it:

```bash
stm32-installer D:/Downloads/ee24-main.zip
```

Only the files the library needs are copied into your project, and the zip is left alone. An unpacked folder works the same way. On a machine with no internet, give it the osal zip as well, `stm32-installer D:/Downloads/ee24-main.zip D:/Downloads/osal-main.zip`, so it does not try to fetch osal. [stm32-installer's README](https://github.com/nimaltd/stm32-installer#installing-a-library) has every option, and how to install on a machine with no internet at all.

### Updating, and pinning a version

Run the same command again. The code is replaced. osal is updated only when ee24 needs a newer one, and your `osal_config.h` is kept either way.

By default you get the newest code on `main`. To hold a project on one release, add `--ref` with a tag, a branch or a commit:

```bash
stm32-installer nimaltd/ee24 --ref v4.0.0
```

### Or copy the files in by hand

1. Copy `src/ee24.h` into your project's `Core/Inc`
2. Copy `src/ee24.c` into your project's `Core/Src`
3. Copy `src/osal.h` and `src/osal_config.h` from [osal](https://github.com/nimaltd/osal) into `Core/Inc`

### Or add the whole repository to a CMake build

If you keep this repository as a submodule rather than installing it:

```cmake
add_subdirectory(osal)     # ee24 needs it, and links it for you
add_subdirectory(ee24)
target_link_libraries(${CMAKE_PROJECT_NAME} nimaltd::ee24)

# ee24 is a static library, so it does not inherit your application's include
# paths and defines, and ee24.h needs main.h and the HAL. A CubeMX project
# keeps them on the stm32cubemx target.
target_link_libraries(ee24 PRIVATE stm32cubemx)
```

The first `target_link_libraries` has no `PRIVATE` on purpose. CubeMX links your application without one, and CMake refuses to mix the two forms on one target. The RTOS setting comes from `osal/src/osal_config.h`, beside `osal.h`.

`stm32-installer` avoids all of this: it writes an INTERFACE target instead, whose sources compile as part of your own target and inherit everything it has.

---

## 🔧 Configuration

ee24 has no settings file of its own. The one setting is the RTOS your project runs, and it lives in your `osal_config.h`, where every NimaLTD library that uses osal reads it:

```c
#define OSAL_RTOS           OSAL_RTOS_NONE
```

| Value | For |
|---|---|
| `OSAL_RTOS_NONE` | Bare metal, no RTOS |
| `OSAL_RTOS_CMSIS_V1` | FreeRTOS through CMSIS-RTOS v1, `cmsis_os.h` |
| `OSAL_RTOS_CMSIS_V2` | FreeRTOS through CMSIS-RTOS v2, `cmsis_os2.h` |
| `OSAL_RTOS_THREADX` | ThreadX, `tx_api.h` |

In CubeMX, FreeRTOS asks which CMSIS-RTOS interface to use when you enable it. Pick the same one here. What changes with an RTOS is described [below](#with-an-rtos).

---

## 🚀 Getting started

Enable I2C in CubeMX, then:

```c
#include "ee24.h"

ee24_t eeprom;

int main(void)
{
    /* ... HAL init, MX_I2C1_Init() ... */

    if (ee24_init(&eeprom, &hi2c1, EE24_ADDRESS_DEFAULT, 256, NULL, 0) == EE24_ERR_NONE)
    {
        uint8_t data[64];

        ee24_read(&eeprom, 0, data, sizeof(data), 100);
    }

    while (1)
    {
    }
}
```

`256` says the chip is a 24C256. The last two arguments are the write protect pin, `NULL` and `0` when it is not wired to the MCU.

Every function returns `EE24_ERR_NONE` when it worked, which is 0. So check for it by name: `if (ee24_read(...))` would mean "if it failed".

### Keeping settings in it

The usual job for an EEPROM is to keep a few settings over a power cycle. A struct goes in and out as it is:

```c
typedef struct
{
    uint32_t magic;       /* tells a written chip from a new one, which reads 0xFF */
    uint16_t volume;
    uint8_t  language;
} settings_t;

settings_t settings;

void settings_load(void)
{
    if ((ee24_read(&eeprom, 0, (uint8_t *)&settings, sizeof(settings), 100) != EE24_ERR_NONE) ||
        (settings.magic != 0x5E771265))
    {
        settings.magic    = 0x5E771265;
        settings.volume   = 50;
        settings.language = 0;
    }
}

void settings_save(void)
{
    ee24_write(&eeprom, 0, (const uint8_t *)&settings, sizeof(settings), 1000);
}
```

An EEPROM cell survives around a million writes. That is a lot for a setting a person changes, and not much for something written every second, so save when a value changes, not on a timer.

### The address

Pick the address from how the chip's A0, A1 and A2 pins are wired. The name lists the pins tied high, and the others are tied to ground, so up to eight chips can share one bus:

| A2 | A1 | A0 | Address | Value |
|---|---|---|---|---|
| GND | GND | GND | `EE24_ADDRESS_DEFAULT` | `0xA0` |
| GND | GND | VCC | `EE24_ADDRESS_A0` | `0xA2` |
| GND | VCC | GND | `EE24_ADDRESS_A1` | `0xA4` |
| GND | VCC | VCC | `EE24_ADDRESS_A0_A1` | `0xA6` |
| VCC | GND | GND | `EE24_ADDRESS_A2` | `0xA8` |
| VCC | GND | VCC | `EE24_ADDRESS_A0_A2` | `0xAA` |
| VCC | VCC | GND | `EE24_ADDRESS_A1_A2` | `0xAC` |
| VCC | VCC | VCC | `EE24_ADDRESS_A0_A1_A2` | `0xAE` |

The value is in the 8 bit form the HAL uses.

The 24C04, 24C08 and 24C16 use some of those pins for the memory address instead: a 24C04 ignores A0, a 24C08 A0 and A1, and a 24C16 all three. `ee24_init()` ignores the same pins, so the address only has to match the pins the chip does use. Fewer of these chips fit on one bus: four 24C04, two 24C08 and one 24C16.

Some makers' 24C01 and 24C02, such as Microchip's 24LC01B and 24LC02B, ignore all three pins and always answer at `0xA0`. Check the datasheet.

### Write protect

With the WP pin wired to the MCU, set it up in CubeMX as a GPIO output, give it a name such as `EE_WP`, and pass it in:

```c
ee24_init(&eeprom, &hi2c1, EE24_ADDRESS_DEFAULT, 256, EE_WP_GPIO_Port, EE_WP_Pin);
```

The chip is protected from `ee24_init()` on. A write lifts the protection only while it runs, and puts it back on every way out, errors included. Not wired, tie WP to ground on the board and pass `NULL` and `0`.

### Timeouts

The last argument of `ee24_read()` and `ee24_write()` is how long the whole call may take, in milliseconds. `HAL_MAX_DELAY` waits as long as it takes.

A read is quick. A write goes a page at a time, 8, 16 or 32 bytes depending on the chip, and each page takes the chip up to 5 ms to store, 10 ms on some older parts. So writing 1 KB to a 24C256 is 32 pages, up to about 160 ms. Give a write plenty of room: when time runs out it stops part way, with the pages before written and the rest not.

### With an RTOS

Set `OSAL_RTOS` in your `osal_config.h` and each chip gets its own mutex, created by `ee24_init()`. Two threads can then use one chip safely: the second waits for the first, and that wait counts against its timeout. While a write waits for the chip, the thread sleeps and your other threads run.

Start the RTOS first. With one set, call every ee24 function from a thread, `ee24_init()` included, and never from `main()` before the kernel starts: the mutex and the sleeps need a running RTOS. So settings are loaded at the top of your first thread:

```c
void StartDefaultTask(void *argument)
{
    ee24_init(&eeprom, &hi2c1, EE24_ADDRESS_DEFAULT, 256, NULL, 0);
    settings_load();

    for (;;)
    {
        /* ... */
    }
}
```

The mutex is per chip, not per bus. Two chips on one bus used from two threads at once would collide on the bus. Usually the HAL refuses the second transfer, which comes back as `EE24_ERR_I2C`, but that is luck rather than safety. Use both chips from one thread, or guard the bus with a mutex of your own.

### What is safe to call from an interrupt

Nothing. A read waits on the bus, a write waits for the chip for milliseconds at a time, and with an RTOS both take a mutex. Hand the work to a task or to the main loop instead, for example with [seq](https://github.com/nimaltd/seq).

---

## 🧰 API

| Function | What it does |
|---|---|
| `ee24_err_t ee24_init(ee24_t *handle, I2C_HandleTypeDef *hi2c, uint8_t dev_address, uint16_t size_kbit, GPIO_TypeDef *wp_port, uint16_t wp_pin)` | Set up a handle for one chip and check that it answers. Call it once per handle |
| `ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len, uint32_t timeout_ms)` | Read `len` bytes from `address` |
| `ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len, uint32_t timeout_ms)` | Write `len` bytes to `address`, and wait until the chip has stored them |

| Error | Means |
|---|---|
| `EE24_ERR_NONE` | Done |
| `EE24_ERR_INVALID` | A `NULL` pointer, a size that is not a 24xx size, or a handle `ee24_init()` did not accept |
| `EE24_ERR_RANGE` | `address + len` runs past the end of the chip. Nothing was sent |
| `EE24_ERR_I2C` | The chip did not answer, or the transfer failed. Check the wiring, the address and the pull ups |
| `EE24_ERR_TIMEOUT` | Time ran out, waiting for the chip or for another thread |
| `EE24_ERR_MUTEX` | The RTOS could not create the mutex, usually a heap that is too small, or refused it, as from an interrupt |

When `ee24_init()` fails, the handle is refused by `ee24_read()` and `ee24_write()` until a later `ee24_init()` succeeds.

---

## 🧪 Running the tests

The tests run on your PC, not on hardware. The HAL calls land on a model of a 24xx chip that behaves as the datasheets describe: a write wraps inside its page, a busy chip does not answer, and WP blocks writes. Time is faked, so a write cycle is tested instantly. You need cmake and any C compiler, nothing else: [Unity](https://github.com/ThrowTheSwitch/Unity) is vendored into `test/unity/`, so there is nothing to install.

One command does everything:

```bash
python test/run_tests.py
```

It configures, builds and runs the suite, then tells you plainly whether it passed. osal is replaced by a fake the tests control, so a mutex held by another thread, or refused by the RTOS, is tested with no RTOS at all. The RTOS side itself is osal's, and osal's own tests cover it. Add `--clean` to start from an empty build folder.

If you prefer doing it by hand:

```bash
cmake -S . -B build -DEE24_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## ⬆️ Coming from version 3

The CubeMX pack is replaced by stm32-installer, and every name changed:

| Was | Is now |
|---|---|
| `EE24_HandleTypeDef` | `ee24_t` |
| `EE24_Init(&h, &hi2c1, address)` | `ee24_init(&h, &hi2c1, address, size_kbit, wp_port, wp_pin)` |
| `EE24_Read(...)`, `EE24_Write(...)` | `ee24_read(...)`, `ee24_write(...)`, same arguments |
| `NimaLTD.I-CUBE-EE24_conf.h` | `osal_config.h`, from [osal](https://github.com/nimaltd/osal) |
| `EE24_SIZE` | the `size_kbit` argument of `ee24_init()` |
| `EE24_USE_WP_PIN` | the `wp_port` and `wp_pin` arguments |
| `EE24_RTOS` | `OSAL_RTOS` |
| `EE24_RTOS_DISABLE` | `OSAL_RTOS_NONE` |

The one to watch: the functions used to return `true` when they worked, and now return `EE24_ERR_NONE`, which is 0. The compiler finds every renamed function for you, but not this:

```c
if (EE24_Read(&ee24, 0, data, 16, 100))                   /* was */
if (ee24_read(&ee24, 0, data, 16, 100) == EE24_ERR_NONE)  /* is now */
if (ee24_read(&ee24, 0, data, 16, 100))                   /* compiles, and means "if it failed" */
```

With an RTOS, ee24 is now called from a thread only, as [above](#with-an-rtos).

You also no longer need CubeMX's "Generate peripheral initialization as a pair of .c/.h files per peripheral": `ee24.h` includes `main.h` now, not `i2c.h`.

---

## 🛠️ Troubleshooting

**`ee24_init()` returns `EE24_ERR_I2C`.** The chip did not acknowledge its address. Check SDA and SCL, the pull up resistors (4.7k to the supply is usual), the supply itself, and that the address matches the A0 to A2 pins.

**STM32F1: the I2C bus is busy from the start.** On the F1, CubeMX enables the I2C clock after it sets up the pins, and some parts then start with a stuck bus. Enable the clock first, in the user code block at the top of `HAL_I2C_MspInit()` in `i2c.c` or `stm32f1xx_hal_msp.c`:

```c
/* USER CODE BEGIN I2C1_MspInit 0 */
__HAL_RCC_I2C1_CLK_ENABLE();
/* USER CODE END I2C1_MspInit 0 */
```

Use `I2C2` in both names for the second bus.

**`EE24_ERR_MUTEX` from `ee24_init()`.** The RTOS could not create the mutex, which almost always means its heap is full. Make `configTOTAL_HEAP_SIZE` bigger in CubeMX's FreeRTOS settings.

---

## 🤝 Contributing

Bug reports and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the style rules and how to run the tests. Nothing to sign, just open a pull request.

---

## 💖 Support

I write these libraries in my own time and give them away, because good tools should be easy to get. If this one saved you an afternoon, there are two things that genuinely help:

**⭐ Star the repo.** It costs you one click, it helps other engineers find the library, and it is the main reason I keep going.

**☕ [Buy me a coffee on Ko-fi](https://ko-fi.com/nimaltd).** Any amount is a real motivation to keep writing, documenting and maintaining this work.

[![GitHub](https://img.shields.io/badge/GitHub-Follow-black?style=for-the-badge&logo=github)](https://github.com/NimaLTD)
[![YouTube](https://img.shields.io/badge/YouTube-Subscribe-red?style=for-the-badge&logo=youtube)](https://youtube.com/@nimaltd)
[![Instagram](https://img.shields.io/badge/Instagram-Follow-purple?style=for-the-badge&logo=instagram)](https://instagram.com/github.nimaltd)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-Connect-blue?style=for-the-badge&logo=linkedin)](https://linkedin.com/in/nimaltd)
[![Email](https://img.shields.io/badge/Email-Contact-red?style=for-the-badge&logo=gmail)](mailto:nima.askari@gmail.com)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-orange?style=for-the-badge&logo=ko-fi)](https://ko-fi.com/nimaltd)

---

## 📜 License

Apache License 2.0. See [LICENSE.md](LICENSE.md).

You are free to use this in commercial and closed source products. What the license asks in return is that you keep the copyright notice and pass along the [NOTICE](NOTICE) file, so the credit travels with the code.

The test folder vendors [Unity](https://github.com/ThrowTheSwitch/Unity) under its own MIT license, kept in [test/unity/LICENSE.txt](test/unity/LICENSE.txt). It is only used for testing and is not part of what you flash to a device.
