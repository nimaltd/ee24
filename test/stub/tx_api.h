/**
 * @file        tx_api.h
 * @brief       Host stub for the ThreadX API.
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

#ifndef TX_API_H
#define TX_API_H

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

#define VOID                        void
#define TX_NULL                     ((void *)0)

#define TX_SUCCESS                  ((UINT)0x00)
#define TX_WAIT_ERROR               ((UINT)0x04)
#define TX_CALLER_ERROR             ((UINT)0x13)
#define TX_MUTEX_ERROR              ((UINT)0x1C)
#define TX_NOT_AVAILABLE            ((UINT)0x1D)

#define TX_INHERIT                  ((UINT)1)
#define TX_WAIT_FOREVER             ((ULONG)0xFFFFFFFFUL)

/* 1000, a common STM32 setting. ThreadX's own default is 100, and a slow tick
   like that is tested in the CMSIS-RTOS v2 build, where the tests can change
   the rate at run time. Both go through the same conversion in ee24.c. */
#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND   ((ULONG)1000)
#endif

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief ThreadX's basic types, as its Cortex-M ports define them.
 */
typedef char          CHAR;
typedef unsigned int  UINT;
typedef unsigned long ULONG;

/*****************************************************************************************************/
/**
 * @brief A mutex. The real one is a control block; this one is enough to count with.
 */
typedef struct
{
    ULONG tx_mutex_id;     /**< Set once created.            */
    UINT  tx_mutex_count;  /**< How many times it is held.   */

} TX_MUTEX;

/*****************************************************************************************************/
/**
 * @brief A thread. Only ever pointed at.
 */
typedef struct
{
    ULONG tx_thread_id;

} TX_THREAD;

/*
 * ****************************************************************************************************
 * Public function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Create a mutex.
 */
UINT tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit);

/*****************************************************************************************************/
/**
 * @brief Take a mutex, waiting up to wait_option ticks.
 */
UINT tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option);

/*****************************************************************************************************/
/**
 * @brief Give a mutex back.
 */
UINT tx_mutex_put(TX_MUTEX *mutex_ptr);

/*****************************************************************************************************/
/**
 * @brief Sleep for timer_ticks.
 */
UINT tx_thread_sleep(ULONG timer_ticks);

/*****************************************************************************************************/
/**
 * @brief The calling thread, or TX_NULL outside any thread.
 */
TX_THREAD *tx_thread_identify(VOID);

#endif /* TX_API_H */
