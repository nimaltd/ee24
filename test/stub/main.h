/**
 * @file        main.h
 * @brief       Host stub standing in for the CubeMX generated main.h and the HAL.
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
 * @note        This file exists only so the library can be compiled and tested
 *              on a PC. It is not part of the shipped library, and it is never
 *              on the include path of a real STM32 build. The HAL functions
 *              here are implemented by the tests, over a model of the chip.
 */

#ifndef MAIN_H
#define MAIN_H

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

/* CMSIS spells volatile this way. */
#define __IO                    volatile

/* As the HAL defines it with USE_FULL_ASSERT set, so the tests see a NULL
   argument stopped. The tests define assert_failed(). */
#define assert_param(expr)      ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))

#define HAL_MAX_DELAY           0xFFFFFFFFU

/* The F4 values. Other families use other numbers, so nothing in the library
   may depend on what they are, and using these catches a library that does. */
#define I2C_MEMADD_SIZE_8BIT    0x00000001U
#define I2C_MEMADD_SIZE_16BIT   0x00000010U

#define GPIO_PIN_5              ((uint16_t)0x0020U)

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief HAL status, with the HAL's own values.
 */
typedef enum
{
    HAL_OK      = 0x00U,
    HAL_ERROR   = 0x01U,
    HAL_BUSY    = 0x02U,
    HAL_TIMEOUT = 0x03U,

} HAL_StatusTypeDef;

/*****************************************************************************************************/
/**
 * @brief GPIO pin state, with the HAL's own values.
 */
typedef enum
{
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET,

} GPIO_PinState;

/*****************************************************************************************************/
/**
 * @brief A GPIO port. The tests only compare its address.
 */
typedef struct
{
    uint32_t ODR;

} GPIO_TypeDef;

/*****************************************************************************************************/
/**
 * @brief An I2C handle. The tests only compare its address.
 */
typedef struct
{
    uint32_t Instance;

} I2C_HandleTypeDef;

/*
 * ****************************************************************************************************
 * Public function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Where a failed assert_param lands. The tests define it.
 */
void assert_failed(uint8_t *file, uint32_t line);

/*****************************************************************************************************/
/**
 * @brief Return the current tick. Backed by a clock the tests control.
 */
uint32_t HAL_GetTick(void);

/*****************************************************************************************************/
/**
 * @brief Wait, by moving the tests' clock on.
 */
void HAL_Delay(uint32_t Delay);

/*****************************************************************************************************/
/**
 * @brief Write to the modelled chip.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                    uint16_t Size, uint32_t Timeout);

/*****************************************************************************************************/
/**
 * @brief Read from the modelled chip.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout);

/*****************************************************************************************************/
/**
 * @brief Ask the modelled chip whether it answers.
 */
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout);

/*****************************************************************************************************/
/**
 * @brief Set a pin. The tests watch the write protect pin through it.
 */
void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);

#endif /* MAIN_H */
