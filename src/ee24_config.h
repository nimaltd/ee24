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
 * @note        Your copy of this file is yours. The installer creates it once
 *              and never touches it again, so updating the library cannot
 *              overwrite a setting you changed.
 */

#ifndef EE24_CONFIG_H
#define EE24_CONFIG_H

/*
 * ****************************************************************************************************
 * Configuration
 * ****************************************************************************************************
*/

/* USER CODE BEGIN EE24_CONFIGURATION */

/* The RTOS your project runs, if any. One of:
     EE24_RTOS_NONE      no RTOS, bare metal
     EE24_RTOS_CMSIS_V1  FreeRTOS through CMSIS-RTOS v1, cmsis_os.h
     EE24_RTOS_CMSIS_V2  FreeRTOS or RTX through CMSIS-RTOS v2, cmsis_os2.h
     EE24_RTOS_THREADX   Azure RTOS ThreadX, tx_api.h
   With an RTOS, each chip gets a mutex so two threads cannot use it at once,
   and a write sleeps rather than spins while the chip finishes. */
#define EE24_RTOS           EE24_RTOS_NONE

/* USER CODE END EE24_CONFIGURATION */

#endif /* EE24_CONFIG_H */
