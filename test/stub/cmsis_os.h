/**
 * @file        cmsis_os.h
 * @brief       Host stub for the CMSIS-RTOS v1 API, as CubeMX generates it for FreeRTOS.
 * @version     4.0.0
 *
 * @author      Nima Askari (NimaLTD)
 * @email       nima.askari@gmail.com
 * @github      https://www.github.com/nimaltd
 *
 * @copyright   (c) 2026 Nima Askari (NimaLTD)
 *              SPDX-License-Identifier: Apache-2.0
 *              See LICENSE.md in the project root for the full license text.
 *
 * @note        Only what ee24 uses, with the real names and values. The
 *              functions are implemented by the tests, over a fake kernel.
 */

#ifndef CMSIS_OS_H
#define CMSIS_OS_H

/*
 * ****************************************************************************************************
 * Includes
 * ****************************************************************************************************
*/

#include <stdint.h>

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

#define osWaitForever           0xFFFFFFFFU

#define osMutexDef(name)        const osMutexDef_t os_mutex_def_##name = { 0U }
#define osMutex(name)           &os_mutex_def_##name

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Status codes, with the values CMSIS-RTOS v1 gives them.
 */
typedef enum
{
    osOK                   = 0x00,
    osErrorParameter       = 0x80,
    osErrorResource        = 0x81,
    osErrorISR             = 0x82,
    osErrorTimeoutResource = 0xC1,
    osErrorOS              = 0xFF,

} osStatus;

/*****************************************************************************************************/
/**
 * @brief Mutex definition, filled in by osMutexDef().
 */
typedef struct
{
    uint32_t dummy;

} osMutexDef_t;

/*****************************************************************************************************/
/**
 * @brief Mutex handle. A pointer, as the FreeRTOS port makes it.
 */
typedef struct test_os_mutex *osMutexId;

/*
 * ****************************************************************************************************
 * Public function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Whether the kernel is running: 1, 0, or -1 when it cannot tell.
 */
int32_t osKernelRunning(void);

/*****************************************************************************************************/
/**
 * @brief Create a mutex.
 */
osMutexId osMutexCreate(const osMutexDef_t *mutex_def);

/*****************************************************************************************************/
/**
 * @brief Take a mutex, waiting up to millisec.
 */
osStatus osMutexWait(osMutexId mutex_id, uint32_t millisec);

/*****************************************************************************************************/
/**
 * @brief Give a mutex back.
 */
osStatus osMutexRelease(osMutexId mutex_id);

/*****************************************************************************************************/
/**
 * @brief Sleep for millisec.
 */
osStatus osDelay(uint32_t millisec);

#endif /* CMSIS_OS_H */
