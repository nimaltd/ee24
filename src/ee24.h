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

#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "osal.h"

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
    EE24_ERR_NONE    = 0, /**< Done.                                                  */
    EE24_ERR_INVALID = 1, /**< An unknown size, or a handle with no ee24_init().     */
    EE24_ERR_RANGE   = 2, /**< The address and length run past the end of the chip.  */
    EE24_ERR_I2C     = 3, /**< The chip did not answer, or the I2C transfer failed.  */
    EE24_ERR_TIMEOUT = 4, /**< Not finished in time, waiting for the mutex included. */
    EE24_ERR_MUTEX   = 5, /**< The RTOS could not create or take the mutex.          */

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
 * @brief Read len bytes starting at address. The timeout covers the whole call.
 */
ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len,
                     uint32_t timeout_ms);

/*****************************************************************************************************/
/**
 * @brief Write len bytes starting at address. The timeout covers the whole call.
 */
ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len,
                      uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* EE24_H */
