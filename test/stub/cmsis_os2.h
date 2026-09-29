/**
 * @file        cmsis_os2.h
 * @brief       Host stub for the CMSIS-RTOS v2 API.
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

#ifndef CMSIS_OS2_H
#define CMSIS_OS2_H

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
#define osMutexPrioInherit      0x00000002U

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Status codes, with the values CMSIS-RTOS v2 gives them.
 */
typedef enum
{
    osOK             = 0,
    osError          = -1,
    osErrorTimeout   = -2,
    osErrorResource  = -3,
    osErrorParameter = -4,
    osErrorNoMemory  = -5,
    osErrorISR       = -6,

} osStatus_t;

/*****************************************************************************************************/
/**
 * @brief Kernel states, with the values CMSIS-RTOS v2 gives them.
 */
typedef enum
{
    osKernelInactive = 0,
    osKernelReady    = 1,
    osKernelRunning  = 2,
    osKernelLocked   = 3,

} osKernelState_t;

/*****************************************************************************************************/
/**
 * @brief Mutex handle.
 */
typedef void *osMutexId_t;

/*****************************************************************************************************/
/**
 * @brief Mutex attributes.
 */
typedef struct
{
    const char *name;      /**< Name, for a debugger.          */
    uint32_t   attr_bits;  /**< osMutexPrioInherit and so on.  */
    void       *cb_mem;    /**< Static control block, or NULL. */
    uint32_t   cb_size;    /**< Size of cb_mem.                */

} osMutexAttr_t;

/*
 * ****************************************************************************************************
 * Public function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief The kernel's state.
 */
osKernelState_t osKernelGetState(void);

/*****************************************************************************************************/
/**
 * @brief Kernel ticks per second.
 */
uint32_t osKernelGetTickFreq(void);

/*****************************************************************************************************/
/**
 * @brief Create a mutex.
 */
osMutexId_t osMutexNew(const osMutexAttr_t *attr);

/*****************************************************************************************************/
/**
 * @brief Take a mutex, waiting up to timeout ticks.
 */
osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);

/*****************************************************************************************************/
/**
 * @brief Give a mutex back.
 */
osStatus_t osMutexRelease(osMutexId_t mutex_id);

/*****************************************************************************************************/
/**
 * @brief Sleep for ticks.
 */
osStatus_t osDelay(uint32_t ticks);

#endif /* CMSIS_OS2_H */
