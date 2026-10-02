# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [4.0.0] - 2026-09-29

### Changed

- **Every public name changed**, to match the other NimaLTD libraries. Code
  written for version 3 will not compile until it is updated:

  | Was | Is now |
  |---|---|
  | `ee24.h`, `ee24.c` at the top of the repository | `src/ee24.h`, `src/ee24.c` |
  | `NimaLTD.I-CUBE-EE24_conf.h` | `osal_config.h`, from [osal](https://github.com/nimaltd/osal) |
  | `EE24_HandleTypeDef` | `ee24_t` |
  | `EE24_Init(&h, &hi2c1, address)` | `ee24_init(&h, &hi2c1, address, size_kbit, wp_port, wp_pin)` |
  | `EE24_Read(...)`, `EE24_Write(...)` | `ee24_read(...)`, `ee24_write(...)`, same arguments |
  | `EE24_SIZE` in the config | the `size_kbit` argument of `ee24_init()` |
  | `EE24_USE_WP_PIN` in the config | the `wp_port` and `wp_pin` arguments, `NULL` and `0` when not wired |
  | `EE24_RTOS` | `OSAL_RTOS` |
  | `EE24_RTOS_DISABLE` | `OSAL_RTOS_NONE` |

- **Every function returns `ee24_err_t` instead of `bool`**, and success is
  `EE24_ERR_NONE`, which is 0. So `if (EE24_Read(...))` becomes
  `if (ee24_read(...) == EE24_ERR_NONE)`. A plain rename to
  `if (ee24_read(...))` still compiles and means the opposite, so check every
  call.
- The chip size is given to `ee24_init()` rather than set once in the config,
  so one project can use two different sizes.
- `ee24.h` includes `main.h` instead of `i2c.h`, so CubeMX no longer has to
  generate a separate `.c` and `.h` file per peripheral.
- A write no longer waits a fixed 10 ms after every page. It asks the chip
  every millisecond and carries on as soon as the chip has stored the page,
  which is how the datasheets say to do it. Writes are usually about twice as
  fast.
- The timeout holds for the whole call: each transfer is given only what is
  left of it. A page of a write used to be given the full timeout on its own.
- `ee24_write()` takes a pointer to `const` data.
- The RTOS is reached through [osal](https://github.com/nimaltd/osal), which the installer
  puts in with ee24, and it is set once in `osal_config.h` for every library
  that uses osal. With an RTOS, every call has to come from a thread once the
  RTOS runs, `ee24_init()` included.
- Installed with [stm32-installer](https://github.com/nimaltd/stm32-installer)
  rather than the STM32CubeMX pack.
- Licence changed from GPLv2 or commercial to Apache-2.0.

### Added

- A real mutex per chip with an RTOS: CMSIS-RTOS v1 and v2 for FreeRTOS, and
  ThreadX, through osal. A thread waiting for another to finish with the chip counts that
  wait against its timeout, and the waits during a write let other threads run.
- Error values that say what went wrong: `EE24_ERR_INVALID`, `EE24_ERR_RANGE`,
  `EE24_ERR_I2C`, `EE24_ERR_TIMEOUT` and `EE24_ERR_MUTEX`.
- An address for each way the A0, A1 and A2 pins can be wired, from
  `EE24_ADDRESS_A0` to `EE24_ADDRESS_A0_A1_A2`, beside `EE24_ADDRESS_DEFAULT`.
- Host unit tests, run against a model of the chip with
  `python test/run_tests.py`.
- CMake build, and a `library.yml` for installing with stm32-installer, from
  GitHub or from a downloaded zip.

### Fixed

- `EE24_Init()` never cleared the lock flag. A handle declared as a local
  variable could start out locked, and the first read or write then waited for
  ever.
- The lock was not a lock. Two threads could both see it free and both take it,
  and a call from an interrupt while the main loop held it never returned.
- With `EE24_RTOS_CMSIS_V1` or `V2`, `ee24.c` included `freertos.h`, which does
  not exist on a case sensitive file system such as Linux or macOS. The file is
  `FreeRTOS.h`, and it is no longer included directly.
- A read of a whole 24C512 in one call failed. 64 KB is one more than the HAL
  can count in a transfer, so it was handed a length of 0 and refused.
- A read or write past the end of the chip wrapped round to its start, and a
  write there overwrote whatever was stored at the beginning. It is now refused
  with `EE24_ERR_RANGE` before anything is sent.
- A read on a 24C04, 24C08 or 24C16 could run from one 256 byte block into the
  next in one transfer, relying on the chip to follow. Reads now stop at every
  block and address the next one properly.
- Reading or writing 0 bytes returned failure. It now succeeds and sends
  nothing.
- On a 24C04, 24C08 or 24C16 given an address with A0, A1 or A2 set where the
  chip puts its block number, every block was sent to the same wrong one: with
  A0 on a 24C04, bytes 0 to 255 were read from and written to 256 to 511.
  `ee24_init()` now ignores those pins, as the chip does.
