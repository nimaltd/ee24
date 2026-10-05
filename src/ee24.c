/**
 * @file        ee24.c
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

/*
 * ****************************************************************************************************
 * Includes
 * ****************************************************************************************************
*/

#include "ee24.h"
#include <stdbool.h>

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

/* How often a write asks the chip whether it has finished storing a page. */
#define EE24_POLL_MS            1U

/* How hard ee24_init() tries to find the chip. */
#define EE24_INIT_TRIALS        2U
#define EE24_INIT_TIMEOUT_MS    100U

/* The HAL counts a transfer in a uint16_t, so a 24C512, 64 KB, cannot be read
   in one. Reads are split at every 32 KB instead. */
#define EE24_READ_LIMIT         0x8000U

/* One address byte reaches 256 bytes. Chips from 24C04 to 24C16 take the rest
   in the I2C address, so one transfer must not cross from one block to the next. */
#define EE24_BLOCK_SIZE         256U

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Where one transfer goes, in the form the HAL takes it.
 */
typedef struct
{
    uint16_t dev_address; /**< I2C address, with any block bits added.    */
    uint16_t mem_address; /**< Address inside the chip, or inside a block. */
    uint16_t mem_size;    /**< I2C_MEMADD_SIZE_8BIT or _16BIT.            */

} ee24_target_t;

/*
 * ****************************************************************************************************
 * Private function prototypes
 * ****************************************************************************************************
*/

static uint32_t      ee24_size_bytes(uint16_t size_kbit);
static uint8_t       ee24_page_size(uint16_t size_kbit);
static uint16_t      ee24_read_size(uint16_t size_kbit);
static uint8_t       ee24_block_bits(uint16_t size_kbit);
static ee24_err_t    ee24_range(const ee24_t *handle, uint32_t address, size_t len);
static uint32_t      ee24_chunk(uint32_t address, size_t left, uint32_t boundary);
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address);
static uint32_t      ee24_remaining(uint32_t start, uint32_t timeout_ms);
static ee24_err_t    ee24_wait_ready(const ee24_t *handle, uint16_t dev_address, uint32_t start,
                                     uint32_t timeout_ms);
static void          ee24_write_protect(const ee24_t *handle, GPIO_PinState state);

/*
 * ****************************************************************************************************
 * Public function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Set up a handle for one chip and check that the chip answers.
 *
 * @param[out] handle       Handle to set up.
 * @param[in]  hi2c         I2C bus the chip is on.
 * @param[in]  dev_address  I2C address in the 8 bit HAL form.
 * @param[in]  size_kbit    Size from the part name, 256 for a 24C256.
 * @param[in]  wp_port      Write protect port, or NULL when WP is not wired.
 * @param[in]  wp_pin       Write protect pin.
 * @return EE24_ERR_NONE, EE24_ERR_INVALID, EE24_ERR_I2C or EE24_ERR_MUTEX.
 */
ee24_err_t ee24_init(ee24_t *handle, I2C_HandleTypeDef *hi2c, uint8_t dev_address,
                     uint16_t size_kbit, GPIO_TypeDef *wp_port, uint16_t wp_pin)
{
    ee24_err_t err  = EE24_ERR_INVALID;
    uint32_t   size = ee24_size_bytes(size_kbit);

    assert_param(handle != NULL);
    assert_param(hi2c != NULL);

    /* Runs once. A step that fails breaks out to the one return at the end. A
       NULL pointer is the caller's bug, left to assert_param above. */
    do
    {
        /* A size of 0 marks the handle as not set up, and read and write
           refuse it. It stays 0 until every step below has worked, so a
           handle whose setup failed half way is never used. */
        handle->size = 0U;

        /* A size that is not a 24xx part is refused here, before anything is
           sent to the bus. */
        if (size == 0U)
        {
            break;
        }

        handle->hi2c    = hi2c;
        handle->wp_port = wp_port;
        handle->wp_pin  = wp_pin;

        /* Everything that depends only on the chip is worked out once, here,
           so a read or a write only has to look it up. The size comes last of
           all, below. */
        handle->page_size = ee24_page_size(size_kbit);
        handle->read_size = ee24_read_size(size_kbit);

        /* From 24C32 up the memory address goes out as two bytes, below that
           as one. */
        handle->mem_size = (size_kbit >= 32U) ? (uint16_t)I2C_MEMADD_SIZE_16BIT
                                              : (uint16_t)I2C_MEMADD_SIZE_8BIT;

        /* A 24C04 to 24C16 ignores the address pins its block number takes,
           so they are cleared here. Left set, a chip with A0 tied high would
           have its first block read from its second. */
        handle->dev_address = (uint8_t)(dev_address & (uint8_t)~ee24_block_bits(size_kbit));

        /* Protected from the start. A write releases it only while it runs. */
        ee24_write_protect(handle, GPIO_PIN_SET);

        /* Does a chip answer at this address? */
        if (HAL_I2C_IsDeviceReady(hi2c, handle->dev_address, EE24_INIT_TRIALS,
                                  EE24_INIT_TIMEOUT_MS) != HAL_OK)
        {
            err = EE24_ERR_I2C;
            break;
        }

        /* The mutex comes last, so a chip that did not answer leaves no mutex
           behind, and calling init again makes only one. Without an RTOS osal
           makes nothing, and with one it most often fails for a heap that is
           too small. */
        if (osal_mutex_create(&handle->mutex) != OSAL_ERR_NONE)
        {
            err = EE24_ERR_MUTEX;
            break;
        }

        /* Only now may ee24_read() and ee24_write() use it. */
        handle->size = size;
        err          = EE24_ERR_NONE;
    }
    while (false);

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Read len bytes starting at address. Not from an interrupt.
 *
 * @param[in,out] handle      Handle from ee24_init().
 * @param[in]     address     First byte to read.
 * @param[out]    data        Where the bytes go.
 * @param[in]     len         Bytes to read, up to the end of the chip.
 * @param[in]     timeout_ms  Time for the whole call. HAL_MAX_DELAY waits for ever.
 * @return EE24_ERR_NONE, or the EE24_ERR_ value that says what went wrong.
 */
ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len,
                     uint32_t timeout_ms)
{
    /* The clock starts now, so a wait for the mutex counts against the
       timeout as well. */
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = EE24_ERR_INVALID;
    osal_err_t lock  = OSAL_ERR_NONE;
    size_t     done  = 0U;

    assert_param(handle != NULL);
    assert_param(data != NULL);

    /* Runs once. A check that fails breaks out to the one return at the end. */
    do
    {
        /* A handle ee24_init() did not accept has a size of 0. A NULL
           pointer is the caller's bug, left to assert_param above. */
        if (handle->size == 0U)
        {
            break;
        }

        /* Nothing goes to the bus for a range past the end of the chip, and
           nothing at all for 0 bytes, which succeeds. */
        err = ee24_range(handle, address, len);

        if ((err != EE24_ERR_NONE) || (len == 0U))
        {
            break;
        }

        /* One thread at a time. Without an RTOS osal takes nothing and this
           always succeeds. */
        lock = osal_mutex_lock(&handle->mutex, timeout_ms);

        if (lock == OSAL_ERR_TIMEOUT)
        {
            /* Another thread kept the chip for the whole wait. */
            err = EE24_ERR_TIMEOUT;
            break;
        }

        if (lock != OSAL_ERR_NONE)
        {
            /* Refused outright, such as from an interrupt. */
            err = EE24_ERR_MUTEX;
            break;
        }

        /* No break from here on: the mutex is held, and is given back below.
           Read in pieces: 24C04 to 24C16 put each 256 byte block at its own
           I2C address, and the HAL cannot count a whole 24C512 at once. */
        while ((err == EE24_ERR_NONE) && (done < len))
        {
            uint32_t      at     = address + (uint32_t)done;
            uint32_t      chunk  = ee24_chunk(at, len - done, handle->read_size);
            ee24_target_t target = ee24_target(handle, at);
            uint32_t      left   = ee24_remaining(start, timeout_ms);

            if (left == 0U)
            {
                /* Out of time before the next piece. */
                err = EE24_ERR_TIMEOUT;
            }
            else if (HAL_I2C_Mem_Read(handle->hi2c, target.dev_address, target.mem_address,
                                      target.mem_size, &data[done], (uint16_t)chunk, left) != HAL_OK)
            {
                /* The HAL gets only what is left of the timeout, so the call
                   as a whole keeps to it. */
                err = EE24_ERR_I2C;
            }
            else
            {
                done += chunk;
            }
        }

        /* Give the chip back to other threads, error or not. */
        osal_mutex_unlock(&handle->mutex);
    }
    while (false);

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Write len bytes starting at address. Not from an interrupt.
 *
 * @param[in,out] handle      Handle from ee24_init().
 * @param[in]     address     First byte to write.
 * @param[in]     data        The bytes to write.
 * @param[in]     len         Bytes to write, up to the end of the chip.
 * @param[in]     timeout_ms  Time for the whole call. HAL_MAX_DELAY waits for ever.
 * @return EE24_ERR_NONE, or the EE24_ERR_ value that says what went wrong. After
 *         an error, the pages before the failing one are written.
 */
ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len,
                      uint32_t timeout_ms)
{
    /* The clock starts now, so a wait for the mutex counts against the
       timeout as well. */
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = EE24_ERR_INVALID;
    osal_err_t lock  = OSAL_ERR_NONE;
    size_t     done  = 0U;

    assert_param(handle != NULL);
    assert_param(data != NULL);

    /* Runs once. A check that fails breaks out to the one return at the end. */
    do
    {
        /* A handle ee24_init() did not accept has a size of 0. A NULL
           pointer is the caller's bug, left to assert_param above. */
        if (handle->size == 0U)
        {
            break;
        }

        /* Nothing goes to the bus for a range past the end of the chip, and
           nothing at all for 0 bytes, which succeeds. */
        err = ee24_range(handle, address, len);

        if ((err != EE24_ERR_NONE) || (len == 0U))
        {
            break;
        }

        /* One thread at a time. Without an RTOS osal takes nothing and this
           always succeeds. */
        lock = osal_mutex_lock(&handle->mutex, timeout_ms);

        if (lock == OSAL_ERR_TIMEOUT)
        {
            /* Another thread kept the chip for the whole wait. */
            err = EE24_ERR_TIMEOUT;
            break;
        }

        if (lock != OSAL_ERR_NONE)
        {
            /* Refused outright, such as from an interrupt. */
            err = EE24_ERR_MUTEX;
            break;
        }

        /* No break from here on: the mutex is held and write protect is off,
           and both are put back below. Write a page at a time. Sent more than
           a page, the chip wraps round inside it and overwrites what it has
           just been given. */
        ee24_write_protect(handle, GPIO_PIN_RESET);

        while ((err == EE24_ERR_NONE) && (done < len))
        {
            uint32_t      at     = address + (uint32_t)done;
            uint32_t      chunk  = ee24_chunk(at, len - done, handle->page_size);
            ee24_target_t target = ee24_target(handle, at);
            uint32_t      left   = ee24_remaining(start, timeout_ms);

            /* The HAL takes a pointer to non-const, but a write only reads
               through it, so casting the const away here is safe. */
            uint8_t *bytes = (uint8_t *)&data[done];

            if (left == 0U)
            {
                /* Out of time before the next page. */
                err = EE24_ERR_TIMEOUT;
            }
            else if (HAL_I2C_Mem_Write(handle->hi2c, target.dev_address, target.mem_address,
                                       target.mem_size, bytes, (uint16_t)chunk, left) != HAL_OK)
            {
                err = EE24_ERR_I2C;
            }
            else
            {
                /* The chip stores the page after the transfer ends, and does
                   not answer anything until it has. So the next page, or the
                   caller's next call, waits for it here. */
                err = ee24_wait_ready(handle, target.dev_address, start, timeout_ms);
                done += chunk;
            }
        }

        /* Protected again and given back on every way out, errors included. */
        ee24_write_protect(handle, GPIO_PIN_SET);
        osal_mutex_unlock(&handle->mutex);
    }
    while (false);

    return err;
}

/*
 * ****************************************************************************************************
 * Private function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Size of the chip in bytes, or 0 for a size that does not exist.
 *
 * @param[in] size_kbit  Size in kilobits.
 * @return Bytes, or 0.
 */
static uint32_t ee24_size_bytes(uint16_t size_kbit)
{
    uint32_t bytes = 0U;

    /* Only the ten sizes a 24xx part comes in, so a mistyped one such as 250
       is refused by ee24_init() rather than used. A kilobit is 128 bytes. */
    switch (size_kbit)
    {
        case 1U:
        case 2U:
        case 4U:
        case 8U:
        case 16U:
        case 32U:
        case 64U:
        case 128U:
        case 256U:
        case 512U:
            bytes = (uint32_t)size_kbit * 128U;
            break;

        default:
            /* Not a 24xx size. */
            break;
    }

    return bytes;
}

/*****************************************************************************************************/
/**
 * @brief How many bytes one write may carry without wrapping inside a page.
 *
 * @param[in] size_kbit  Size in kilobits, already checked.
 * @return Page size in bytes.
 */
static uint8_t ee24_page_size(uint16_t size_kbit)
{
    uint8_t page = 32U;

    if (size_kbit <= 2U)
    {
        /* 24C01 and 24C02. Some makers use 16, but 8 is safe on all of them. */
        page = 8U;
    }
    else if (size_kbit <= 16U)
    {
        /* 24C04 to 24C16. */
        page = 16U;
    }
    else
    {
        /* 32 bytes from 24C32 up. From 24C128 the real page is 64 or 128,
           whole multiples of 32, so 32 is safe there too, only slower. */
    }

    return page;
}

/*****************************************************************************************************/
/**
 * @brief How many bytes one read may carry, as a boundary it must not cross.
 *
 * @param[in] size_kbit  Size in kilobits, already checked.
 * @return The boundary in bytes.
 */
static uint16_t ee24_read_size(uint16_t size_kbit)
{
    /* The HAL counts a transfer in 16 bits, so a 24C512, 64 KB, is read in
       two halves. Every smaller chip fits in one. */
    uint16_t limit = EE24_READ_LIMIT;

    /* 24C04 to 24C16 put each 256 byte block at its own I2C address, so a
       read must stop at the end of a block. */
    if ((size_kbit >= 4U) && (size_kbit <= 16U))
    {
        limit = EE24_BLOCK_SIZE;
    }

    return limit;
}

/*****************************************************************************************************/
/**
 * @brief The address pins a chip ignores, because its block number goes there instead.
 *
 * @param[in] size_kbit  Size in kilobits, already checked.
 * @return The bits of the 8 bit I2C address the chip uses for its block number.
 */
static uint8_t ee24_block_bits(uint16_t size_kbit)
{
    uint8_t bits = 0U;

    /* One address byte reaches 256 bytes, so 24C04 to 24C16 answer at one I2C
       address per 256 byte block, the block number sitting where A0 to A2
       would be. Bit 1 is A0, bit 2 A1 and bit 3 A2. */
    switch (size_kbit)
    {
        case 4U:
            /* Two blocks, in place of A0. */
            bits = 0x02U;
            break;

        case 8U:
            /* Four blocks, in place of A0 and A1. */
            bits = 0x06U;
            break;

        case 16U:
            /* Eight blocks, in place of all three. */
            bits = 0x0EU;
            break;

        default:
            /* One block, or a two byte memory address: every pin counts. */
            break;
    }

    return bits;
}

/*****************************************************************************************************/
/**
 * @brief Check that a transfer stays inside the chip.
 *
 * @param[in] handle   Handle of the chip.
 * @param[in] address  First byte of the transfer.
 * @param[in] len      Length of the transfer.
 * @return EE24_ERR_NONE or EE24_ERR_RANGE.
 */
static ee24_err_t ee24_range(const ee24_t *handle, uint32_t address, size_t len)
{
    ee24_err_t err = EE24_ERR_NONE;

    /* Written as a subtraction, because address + len can wrap past zero and
       then look small enough. Sent anyway, the chip would wrap round and
       overwrite its own start. */
    if ((address > handle->size) || (len > (size_t)(handle->size - address)))
    {
        err = EE24_ERR_RANGE;
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief How much of what is left fits before the next boundary.
 *
 * @param[in] address   Where this piece starts.
 * @param[in] left      Bytes still to go.
 * @param[in] boundary  Page or block size.
 * @return Bytes for this piece.
 */
static uint32_t ee24_chunk(uint32_t address, size_t left, uint32_t boundary)
{
    /* From here to the next boundary, so the first piece of a transfer that
       starts inside a page only fills the rest of that page. */
    uint32_t room  = boundary - (address % boundary);
    uint32_t chunk = room;

    /* Or less, when that is all there is left. */
    if (left < (size_t)room)
    {
        chunk = (uint32_t)left;
    }

    return chunk;
}

/*****************************************************************************************************/
/**
 * @brief Work out the I2C address and memory address for one transfer.
 *
 * @param[in] handle   Handle of the chip.
 * @param[in] address  Memory address, already checked.
 * @return What the HAL needs to reach that address.
 */
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address)
{
    ee24_target_t target;

    /* Only the address changes from one piece to the next. What depends on
       the chip alone was worked out by ee24_init(). */
    target.mem_size = handle->mem_size;

    if (handle->mem_size == (uint16_t)I2C_MEMADD_SIZE_16BIT)
    {
        /* Two address bytes reach the whole chip, at one I2C address. */
        target.dev_address = handle->dev_address;
        target.mem_address = (uint16_t)address;
    }
    else
    {
        /* One byte reaches 256 bytes. 24C04 to 24C16 take the higher bits in
           the I2C address, where A0 to A2 would be: bit 8 of the memory
           address lands in bit 1 of the 8 bit I2C address, and so on up.
           ee24_init() cleared those bits, so an OR puts them in. On a 24C01 or
           24C02 they are always 0. */
        target.dev_address = (uint16_t)(handle->dev_address | ((address >> 7U) & 0x0EU));
        target.mem_address = (uint16_t)(address & 0xFFU);
    }

    return target;
}

/*****************************************************************************************************/
/**
 * @brief Milliseconds left of a timeout.
 *
 * @param[in] start       HAL_GetTick() when the call began.
 * @param[in] timeout_ms  The caller's timeout.
 * @return Milliseconds left, or 0 once it has run out.
 */
static uint32_t ee24_remaining(uint32_t start, uint32_t timeout_ms)
{
    /* A subtraction, which stays right when the tick wraps after 49 days. */
    uint32_t elapsed = HAL_GetTick() - start;
    uint32_t left    = 0U;

    if (elapsed < timeout_ms)
    {
        left = timeout_ms - elapsed;
    }

    return left;
}

/*****************************************************************************************************/
/**
 * @brief Wait until the chip has stored the page it was just sent.
 *
 * @param[in] handle       Handle of the chip.
 * @param[in] dev_address  I2C address the page went to.
 * @param[in] start        HAL_GetTick() when the call began.
 * @param[in] timeout_ms   The caller's timeout.
 * @return EE24_ERR_NONE or EE24_ERR_TIMEOUT.
 */
static ee24_err_t ee24_wait_ready(const ee24_t *handle, uint16_t dev_address, uint32_t start,
                                  uint32_t timeout_ms)
{
    ee24_err_t err  = EE24_ERR_TIMEOUT;
    uint32_t   left = 0U;

    do
    {
        /* Sleep first: no chip stores a page in under a millisecond, so
           asking straight away would only waste a question. With an RTOS
           other threads run meanwhile, and without one it is HAL_Delay(). */
        osal_delay_ms(EE24_POLL_MS);

        left = ee24_remaining(start, timeout_ms);

        /* A chip busy storing a page does not acknowledge its address, so an
           answer means it has finished. This is the method the datasheets
           give, and it is usually well under the 5 to 10 ms a fixed wait
           would have to allow. */
        if ((left > 0U) && (HAL_I2C_IsDeviceReady(handle->hi2c, dev_address, 1U, left) == HAL_OK))
        {
            err = EE24_ERR_NONE;
        }
    }
    while ((err != EE24_ERR_NONE) && (left > 0U));

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Drive the write protect pin, when there is one.
 *
 * @param[in] handle  Handle of the chip.
 * @param[in] state   GPIO_PIN_SET protects the chip, GPIO_PIN_RESET allows writes.
 */
static void ee24_write_protect(const ee24_t *handle, GPIO_PinState state)
{
    /* NULL means WP is not wired to the MCU, so there is nothing to drive. */
    if (handle->wp_port != NULL)
    {
        HAL_GPIO_WritePin(handle->wp_port, handle->wp_pin, state);
    }
}
