# Contributing

Thanks for wanting to help. Bug reports, fixes and new features are all welcome.
There is no contributor agreement to sign: under section 5 of the Apache Licence
2.0, anything you submit for inclusion is covered by the project's own licence.

## Reporting a bug

Open an issue and include the STM32 family you are using, the exact EEPROM part
number, the `size_kbit` and address you pass to `ee24_init()`, your
`ee24_config.h` settings, the error value you got back, and the smallest piece
of code that shows the problem. A failing test is even better, see below.

## Making a change

1. Add or update a test in `test/test_ee24.c` that fails before your change and
   passes after it. The HAL calls land on a model of a real chip, in the same
   file, so most chip behaviour can be tested without hardware. If a change
   cannot be covered by a test, say why in the pull request.
2. Run the tests:

   ```bash
   python test/run_tests.py
   ```

   This builds and runs the suite four times: once with the shipped
   `ee24_config.h`, and once for each RTOS setting.

3. Match the existing code style. The short version: 4 spaces and no tabs, Allman
   braces, `snake_case`, every file scope name prefixed with `ee24_`, a Doxygen
   block on every public function, section banners at 103 columns, and comments
   in plain 7-bit ASCII with no em dashes. A `.clang-format` in the project root
   handles the mechanical parts:

   ```bash
   clang-format -i src/ee24.c src/ee24.h
   ```

4. Keep vendor code out of the library. `src/ee24.h` includes only the standard
   headers, `ee24_config.h`, `main.h` for the HAL's types, and the RTOS header
   `ee24_config.h` asks for. That is what lets the tests replace all of them on
   a PC.

## What CI checks

Every pull request runs two jobs, and both must be green:

- the full test suite on Ubuntu, in all four builds, with `-Wall -Wextra -Wpedantic -Werror`
- a cross compile of `src/ee24.c` for Cortex-M4 with the same warning settings,
  once for each RTOS setting

Warnings are errors, so a build that warns will not merge.

## Questions

Open an issue, or reach me at nima.askari@gmail.com.
