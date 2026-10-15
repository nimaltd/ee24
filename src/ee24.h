/**
 * @file        ee24.h
 * @brief       Driver for 24xx I2C EEPROMs, 24C01 to 24C512, on the STM32 HAL.
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
 */

#ifndef EE24_H
#define EE24_H

/*
 * ****************************************************************************************************
 * Includes
 * ****************************************************************************************************
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "osal.h"
#include "ee24_config.h"

/*
 * ****************************************************************************************************
 * Configuration checks
 * ****************************************************************************************************
*/

/* Checked here and never in ee24_config.h. The part of that file between its
   USER CODE markers is the user's and survives every update, so a check in it
   could be edited away, and one added later would never reach it. */

/* The values EE24_TRANSFER can take. They start at 1 on purpose: a misspelt
   name counts as 0 inside #if, and 0 is then refused below instead of quietly
   meaning one of them. */
#define EE24_TRANSFER_POLLING   1
#define EE24_TRANSFER_DMA       2

#ifndef EE24_TRANSFER
#error "EE24_TRANSFER is not defined. Add it to ee24_config.h"
#elif (EE24_TRANSFER != EE24_TRANSFER_POLLING) && (EE24_TRANSFER != EE24_TRANSFER_DMA)
#error "EE24_TRANSFER must be EE24_TRANSFER_POLLING or EE24_TRANSFER_DMA"
#endif

/* Added after the first version, so a configuration file from before has none
   and gets the default. A piece is one HAL transfer, which counts in 16 bits,
   and reads are split at 32 KB anyway. */
#ifndef EE24_POLL_CHUNK
#define EE24_POLL_CHUNK                 32U
#elif (EE24_POLL_CHUNK < 1) || (EE24_POLL_CHUNK > 32768)
#error "EE24_POLL_CHUNK must be from 1 to 32768"
#endif

/* 0 is allowed here: it tries for the mutex once and does not wait. */
#ifndef EE24_TIMEOUT_MUTEX_MS
#error "EE24_TIMEOUT_MUTEX_MS is not defined. Add it to ee24_config.h"
#endif

/* The others must allow some time, or every call would time out. The transfer
   time is multiplied by up to 128, for a 32 KB read, so a bigger one would
   wrap round to a short time. */
#ifndef EE24_TIMEOUT_TRANSFER_MS
#error "EE24_TIMEOUT_TRANSFER_MS is not defined. Add it to ee24_config.h"
#elif EE24_TIMEOUT_TRANSFER_MS == 0
#error "EE24_TIMEOUT_TRANSFER_MS must be greater than 0"
#elif (EE24_TIMEOUT_TRANSFER_MS > 1000000U) && (EE24_TIMEOUT_TRANSFER_MS != HAL_MAX_DELAY)
#error "EE24_TIMEOUT_TRANSFER_MS must be at most 1000000, or HAL_MAX_DELAY to wait for ever"
#endif

#ifndef EE24_TIMEOUT_PAGE_MS
#error "EE24_TIMEOUT_PAGE_MS is not defined. Add it to ee24_config.h"
#elif EE24_TIMEOUT_PAGE_MS == 0
#error "EE24_TIMEOUT_PAGE_MS must be greater than 0"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

/* The I2C address for each way the A0, A1 and A2 pins can be wired, in the 8 bit
   form the HAL expects. The name lists the pins tied high, and the others are
   tied to ground. A 24C04, 24C08 or 24C16 ignores some of the pins, and
   ee24_init() ignores them too. */
#define EE24_ADDRESS_DEFAULT    0xA0U
#define EE24_ADDRESS_A0         0xA2U
#define EE24_ADDRESS_A1         0xA4U
#define EE24_ADDRESS_A0_A1      0xA6U
#define EE24_ADDRESS_A2         0xA8U
#define EE24_ADDRESS_A0_A2      0xAAU
#define EE24_ADDRESS_A1_A2      0xACU
#define EE24_ADDRESS_A0_A1_A2   0xAEU

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Error values returned by every function.
 */
typedef enum
{
    EE24_ERR_NONE    = 0, /**< Done, or for a job, started: see ee24_wait().          */
    EE24_ERR_INVALID = 1, /**< An unknown size, or a handle with no ee24_init().     */
    EE24_ERR_RANGE   = 2, /**< The address and length run past the end of the chip.  */
    EE24_ERR_I2C     = 3, /**< The chip did not answer, or the I2C transfer failed.  */
    EE24_ERR_TIMEOUT = 4, /**< Not finished in the time ee24_config.h allows.        */
    EE24_ERR_MUTEX   = 5, /**< The RTOS could not create or take the mutex.          */
    EE24_ERR_BUSY    = 6, /**< A job is still running: see ee24_process().           */

} ee24_err_t;

/*****************************************************************************************************/
/**
 * @brief One EEPROM chip. Declare one per chip and hand it to ee24_init().
 */
typedef struct
{
    I2C_HandleTypeDef *hi2c;        /**< The bus the chip is on.                        */
    GPIO_TypeDef      *wp_port;     /**< Write protect port, NULL when not wired.       */
    uint32_t          size;         /**< Bytes in the chip, 0 until ee24_init() works.  */
    uint16_t          wp_pin;       /**< Write protect pin.                             */
    uint16_t          read_size;    /**< Most one read may carry: a block, or 32 KB.    */
    uint16_t          mem_size;     /**< I2C_MEMADD_SIZE_8BIT or _16BIT.                */
    uint8_t           page_size;    /**< Most one write may carry without wrapping.     */
    uint8_t           dev_address;  /**< I2C address, with the pins the chip ignores 0. */
    uint8_t           job_state;    /**< What the running job is doing.                 */
    ee24_err_t        job_result;   /**< How the last job ended, for ee24_wait().       */
    uint8_t           *job_data;    /**< The caller's buffer.                           */
    size_t            job_len;      /**< Bytes in the whole job.                        */
    size_t            job_done;     /**< Bytes finished so far.                         */
    uint32_t          job_address;  /**< Where in the chip the job starts.              */
    uint32_t          job_step;     /**< Bytes in the piece under way.                  */
    uint32_t          job_start;    /**< HAL_GetTick() when that piece started.         */
    uint32_t          job_timeout;  /**< Time that piece may take.                      */
    osal_mutex_t      mutex;        /**< Lets one thread use the chip at a time.        */

} ee24_t;

/*
 * ****************************************************************************************************
 * Public function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Set up a handle for one chip and check that the chip answers.
 * @note  Call it once per handle. With an RTOS, only from a thread once the RTOS runs.
 */
ee24_err_t ee24_init(ee24_t *handle, I2C_HandleTypeDef *hi2c, uint8_t dev_address,
                     uint16_t size_kbit, GPIO_TypeDef *wp_port, uint16_t wp_pin);

/*****************************************************************************************************/
/**
 * @brief Read len bytes at address, and return once they are in.
 */
ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len);

/*****************************************************************************************************/
/**
 * @brief Write len bytes at address, and return once the chip has stored them.
 */
ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len);

/*****************************************************************************************************/
/**
 * @brief Start reading len bytes at address, as a job. Returns at once.
 */
ee24_err_t ee24_read_nb(ee24_t *handle, uint32_t address, uint8_t *data, size_t len);

/*****************************************************************************************************/
/**
 * @brief Start writing len bytes at address, as a job. Returns at once.
 */
ee24_err_t ee24_write_nb(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len);

/*****************************************************************************************************/
/**
 * @brief Move the running job one step, and say whether it still runs. Returns at once.
 */
bool ee24_process(ee24_t *handle);

/*****************************************************************************************************/
/**
 * @brief Wait until the running job has ended, and say how it ended.
 */
ee24_err_t ee24_wait(ee24_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* EE24_H */
