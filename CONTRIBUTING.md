# Contributing

Thanks for wanting to help. Bug reports, fixes and new features are all welcome.
There is no contributor agreement to sign: under section 5 of the Apache Licence
2.0, anything you submit for inclusion is covered by the project's own licence.

## Reporting a bug

Open an issue and include the STM32 family you are using, the exact EEPROM part
number, the `size_kbit` and address you pass to `ee24_init()`, the RTOS set in
your `osal_config.h`, the error value you got back, and the smallest piece of
code that shows the problem. A failing test is even better, see below.

## Making a change

1. Add or update a test in `test/test_ee24.c` that fails before your change and
   passes after it. The HAL calls land on a model of a real chip, in the same
   file, so most chip behaviour can be tested without hardware. osal is a fake
   in `test/fake/osal.h`, implemented in the same file, so a test can hold or
   refuse the mutex. If a change cannot be covered by a test, say why in the
   pull request.
2. Run the tests:

   ```bash
   python test/run_tests.py
   ```

   This builds and runs the suite. There is one build: ee24 reaches the RTOS
   only through osal, and osal's own tests cover each RTOS.

3. Match the existing code style. The short version: 4 spaces and no tabs, Allman
   braces, `snake_case`, every file scope name prefixed with `ee24_`, a Doxygen
   block on every public function, section banners at 103 columns, and comments
   in plain 7-bit ASCII with no em dashes. A `.clang-format` in the project root
   handles the mechanical parts:

   ```bash
   clang-format -i src/ee24.c src/ee24.h
   ```

4. Keep vendor code out of the library. `src/ee24.h` includes only the standard
   headers, `main.h` for the HAL's types, and `osal.h` for the mutex and the
   waits. Nothing RTOS specific belongs in ee24: it goes in osal. That is what
   lets the tests replace all of them on a PC.

## What CI checks

Every pull request runs two jobs, and both must be green:

- the full test suite on Ubuntu, with `-Wall -Wextra -Wpedantic -Werror`
- a cross compile of `src/ee24.c` for Cortex-M4 with the same warning settings,
  against the real osal, once for each RTOS setting

Warnings are errors, so a build that warns will not merge.

## Questions

Open an issue, or reach me at nima.askari@gmail.com.
