/**
 * @file        ee24_config.h
 * @brief       Build time configuration for the ee24 library.
 * @version     4.0.0
 *
 * @author      Nima Askari (NimaLTD)
 * @email       nima.askari@gmail.com
 * @github      https://www.github.com/nimaltd
 * @linkedin    https://www.linkedin.com/in/nimaltd
 * @youtube     https://www.youtube.com/@nimaltd
 * @instagram   https://instagram.com/github.nimaltd
 *
 * @copyright   (c) 2026 Nima Askari (NimaLTD)
 *              SPDX-License-Identifier: Apache-2.0
 *              See LICENSE.md in the project root for the full license text.
 *
 * @note        Your settings go between the USER CODE markers. The installer
 *              replaces the rest of this file on every update and keeps what
 *              is between them, so a setting you changed is never lost.
 */

#ifndef EE24_CONFIG_H
#define EE24_CONFIG_H

/*
 * ****************************************************************************************************
 * Configuration
 * ****************************************************************************************************
*/

/* USER CODE BEGIN EE24_CONFIGURATION */

/* How ee24_read() and ee24_write() move their data. One of:
     EE24_TRANSFER_POLLING  the CPU moves it, and a read returns with the data
     EE24_TRANSFER_DMA      the DMA moves it, and a read is a job like a write:
                            it returns once started, and ee24_is_busy() or
                            ee24_wait() says when it has finished
   With DMA, give the I2C a DMA channel for TX and one for RX in CubeMX, and
   turn on the I2C event and error interrupts. */
#define EE24_TRANSFER                   EE24_TRANSFER_POLLING

/* How long each step may take, in milliseconds, before it ends with
   EE24_ERR_TIMEOUT. HAL_MAX_DELAY in any of them waits for ever. */

/* Waiting for another thread to finish with the chip. HAL_MAX_DELAY waits as
   long as it takes, which is safe: whoever has the chip gives it back within
   the times below. */
#define EE24_TIMEOUT_MUTEX_MS           HAL_MAX_DELAY

/* Moving data on the bus, for every 256 bytes a transfer carries. 256 bytes
   take 23 ms at 100 kHz, the slowest clock the chips are made for, and 6 ms at
   400 kHz. A page gets this once, and a 32 KB read, 128 times as much, gets it
   128 times. At most 1000000. */
#define EE24_TIMEOUT_TRANSFER_MS        35U

/* Storing one page: 5 ms at most on most chips, 10 ms on some older ones. */
#define EE24_TIMEOUT_PAGE_MS            15U

/* USER CODE END EE24_CONFIGURATION */

#endif /* EE24_CONFIG_H */
