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

/* The largest finite RTOS wait. One more is "wait forever" in all three APIs. */
#define EE24_TICKS_MAX          0xFFFFFFFEU

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
 * Constants
 * ****************************************************************************************************
*/

#if EE24_RTOS == EE24_RTOS_CMSIS_V1
/* One definition serves every chip. With no control block in it, each
   osMutexCreate() call allocates a mutex of its own. */
static osMutexDef(ee24_mutex);
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
/* Priority inheritance, so a low priority thread in the middle of a write is
   not held up by a medium one while a high priority thread waits for the chip. */
static const osMutexAttr_t ee24_mutex_attr = { "ee24", osMutexPrioInherit, NULL, 0U };
#endif

/*
 * ****************************************************************************************************
 * Private function prototypes
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Size of the chip in bytes, or 0 for a size that does not exist.
 */
static uint32_t ee24_size_bytes(uint16_t size_kbit);

/*****************************************************************************************************/
/**
 * @brief How many bytes one write may carry without wrapping inside a page.
 */
static uint32_t ee24_page_size(uint16_t size_kbit);

/*****************************************************************************************************/
/**
 * @brief How many bytes one read may carry.
 */
static uint32_t ee24_read_size(uint16_t size_kbit);

/*****************************************************************************************************/
/**
 * @brief Check the arguments every read and write share.
 */
static ee24_err_t ee24_check(const ee24_t *handle, uint32_t address, const void *data, size_t len);

/*****************************************************************************************************/
/**
 * @brief How much of what is left fits before the next boundary.
 */
static uint32_t ee24_chunk(uint32_t address, size_t left, uint32_t boundary);

/*****************************************************************************************************/
/**
 * @brief Work out the I2C address and memory address for one transfer.
 */
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address);

/*****************************************************************************************************/
/**
 * @brief Milliseconds left of a timeout that started at start, or 0 once it has run out.
 */
static uint32_t ee24_remaining(uint32_t start, uint32_t timeout_ms);

/*****************************************************************************************************/
/**
 * @brief Wait until the chip has stored the page it was just sent.
 */
static ee24_err_t ee24_wait_ready(const ee24_t *handle, uint16_t dev_address, uint32_t start,
                                  uint32_t timeout_ms);

/*****************************************************************************************************/
/**
 * @brief Drive the write protect pin, when there is one.
 */
static void ee24_write_protect(const ee24_t *handle, GPIO_PinState state);

/*****************************************************************************************************/
/**
 * @brief Create the handle's mutex. Does nothing without an RTOS.
 */
static ee24_err_t ee24_mutex_create(ee24_t *handle);

/*****************************************************************************************************/
/**
 * @brief Take the handle's mutex, when the RTOS is running.
 */
static ee24_err_t ee24_lock(ee24_t *handle, uint32_t timeout_ms);

/*****************************************************************************************************/
/**
 * @brief Give back what ee24_lock() took.
 */
static void ee24_unlock(ee24_t *handle);

/*****************************************************************************************************/
/**
 * @brief Wait EE24_POLL_MS, letting other threads run when there is an RTOS.
 */
static void ee24_sleep(void);

#if EE24_RTOS != EE24_RTOS_NONE
/*****************************************************************************************************/
/**
 * @brief Whether the RTOS is running, so a thread may block.
 */
static bool ee24_kernel_running(void);
#endif

#if (EE24_RTOS == EE24_RTOS_CMSIS_V2) || (EE24_RTOS == EE24_RTOS_THREADX)
/*****************************************************************************************************/
/**
 * @brief Turn milliseconds into RTOS ticks, rounding up.
 */
static uint32_t ee24_ticks(uint32_t ms, uint32_t tick_hz);
#endif

/*
 * ****************************************************************************************************
 * Public function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Set up a handle for one chip and check that the chip answers.
 *
 * The size is the number in the part name, in kilobits: 2 for a 24C02, 256 for
 * a 24C256. It decides how addresses are sent and how big a write can be, so
 * it has to match the chip. The write protect pin is driven high here, which
 * protects the chip, and is pulled low only while a write runs. Pass NULL and
 * 0 when WP is not wired to the MCU, and tie it to ground on the board.
 *
 * Call it once per handle. With an RTOS it creates the handle's mutex, and a
 * second call would create a second one. The mutex is made only once the chip
 * has answered, so calling it again after a failure is fine.
 *
 * @param[out] handle       Handle to set up. Must not be NULL.
 * @param[in]  hi2c         The I2C bus the chip is on. Must not be NULL.
 * @param[in]  dev_address  I2C address in the 8 bit HAL form, EE24_ADDRESS_DEFAULT
 *                          when A0, A1 and A2 are tied to ground.
 * @param[in]  size_kbit    1, 2, 4, 8, 16, 32, 64, 128, 256 or 512.
 * @param[in]  wp_port      Port of the write protect pin, or NULL when not wired.
 * @param[in]  wp_pin       Write protect pin, such as GPIO_PIN_5. Ignored when
 *                          wp_port is NULL.
 * @return EE24_ERR_NONE when the chip answered, EE24_ERR_INVALID for a NULL
 *         pointer or an unknown size, EE24_ERR_I2C when the chip did not answer,
 *         or EE24_ERR_MUTEX when the RTOS could not create the mutex. On any
 *         error the handle is refused by ee24_read() and ee24_write().
 */
ee24_err_t ee24_init(ee24_t *handle, I2C_HandleTypeDef *hi2c, uint8_t dev_address,
                     uint16_t size_kbit, GPIO_TypeDef *wp_port, uint16_t wp_pin)
{
    ee24_err_t err = EE24_ERR_INVALID;

    assert_param(handle != NULL);
    assert_param(hi2c != NULL);

    if (handle != NULL)
    {
        /* Cleared first, so a handle whose setup failed half way is refused
           rather than used. */
        handle->ready = 0U;

        if ((hi2c != NULL) && (ee24_size_bytes(size_kbit) != 0U))
        {
            handle->hi2c        = hi2c;
            handle->dev_address = dev_address;
            handle->size_kbit   = size_kbit;
            handle->wp_port     = wp_port;
            handle->wp_pin      = wp_pin;

            /* Protected from the start. A write releases it only while it runs. */
            ee24_write_protect(handle, GPIO_PIN_SET);

            if (HAL_I2C_IsDeviceReady(hi2c, dev_address, EE24_INIT_TRIALS, EE24_INIT_TIMEOUT_MS)
                != HAL_OK)
            {
                err = EE24_ERR_I2C;
            }
            else
            {
                err = ee24_mutex_create(handle);
            }

            if (err == EE24_ERR_NONE)
            {
                handle->ready = 1U;
            }
        }
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Read len bytes starting at address.
 *
 * Any length is fine, up to the end of the chip: the read is split wherever the
 * chip or the HAL needs it to be. It blocks until the data is in, or until the
 * timeout runs out. With an RTOS, a thread waiting for another one to finish
 * with the chip counts that wait against the same timeout.
 *
 * Not for use from an interrupt: it waits on the I2C bus, and with an RTOS it
 * takes a mutex.
 *
 * @param[in,out] handle      Handle from ee24_init(). Must not be NULL.
 * @param[in]     address     First byte to read, from 0.
 * @param[out]    data        Where the bytes go. Must not be NULL.
 * @param[in]     len         How many bytes to read. 0 does nothing.
 * @param[in]     timeout_ms  Time allowed for the whole call. HAL_MAX_DELAY waits
 *                            for as long as it takes.
 * @return EE24_ERR_NONE when every byte was read, EE24_ERR_INVALID for a NULL
 *         pointer or a handle ee24_init() did not accept, EE24_ERR_RANGE when
 *         address + len runs past the end of the chip, EE24_ERR_I2C when a
 *         transfer failed, EE24_ERR_TIMEOUT when time ran out, or EE24_ERR_MUTEX
 *         when the RTOS refused the mutex.
 */
ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len,
                     uint32_t timeout_ms)
{
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = ee24_check(handle, address, data, len);

    if ((err == EE24_ERR_NONE) && (len > 0U))
    {
        err = ee24_lock(handle, timeout_ms);

        if (err == EE24_ERR_NONE)
        {
            uint32_t limit = ee24_read_size(handle->size_kbit);
            size_t   done  = 0U;

            while ((err == EE24_ERR_NONE) && (done < len))
            {
                uint32_t      at     = address + (uint32_t)done;
                uint32_t      chunk  = ee24_chunk(at, len - done, limit);
                ee24_target_t target = ee24_target(handle, at);
                uint32_t      left   = ee24_remaining(start, timeout_ms);

                if (left == 0U)
                {
                    err = EE24_ERR_TIMEOUT;
                }
                else if (HAL_I2C_Mem_Read(handle->hi2c, target.dev_address, target.mem_address,
                                          target.mem_size, &data[done], (uint16_t)chunk, left)
                         != HAL_OK)
                {
                    err = EE24_ERR_I2C;
                }
                else
                {
                    done += chunk;
                }
            }

            ee24_unlock(handle);
        }
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Write len bytes starting at address.
 *
 * Any length is fine, up to the end of the chip. The chip stores one page at a
 * time, so the data goes out a page at a time, and after each one the chip is
 * asked every millisecond whether it has finished. When this returns, the chip
 * is ready for the next call.
 *
 * With a write protect pin, the chip is unprotected only while this runs, and
 * protected again on every way out, errors included. With an RTOS, the waits
 * between pages let other threads run.
 *
 * Not for use from an interrupt: a page takes a few milliseconds to store, and
 * with an RTOS it takes a mutex.
 *
 * @param[in,out] handle      Handle from ee24_init(). Must not be NULL.
 * @param[in]     address     First byte to write, from 0.
 * @param[in]     data        The bytes to write. Must not be NULL.
 * @param[in]     len         How many bytes to write. 0 does nothing.
 * @param[in]     timeout_ms  Time allowed for the whole call. HAL_MAX_DELAY waits
 *                            for as long as it takes.
 * @return EE24_ERR_NONE when every byte was stored, EE24_ERR_INVALID for a NULL
 *         pointer or a handle ee24_init() did not accept, EE24_ERR_RANGE when
 *         address + len runs past the end of the chip, EE24_ERR_I2C when a
 *         transfer failed, EE24_ERR_TIMEOUT when time ran out, or EE24_ERR_MUTEX
 *         when the RTOS refused the mutex. After an error, the pages before the
 *         failing one are written and the rest are not.
 */
ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len,
                      uint32_t timeout_ms)
{
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = ee24_check(handle, address, data, len);

    if ((err == EE24_ERR_NONE) && (len > 0U))
    {
        err = ee24_lock(handle, timeout_ms);

        if (err == EE24_ERR_NONE)
        {
            uint32_t page = ee24_page_size(handle->size_kbit);
            size_t   done = 0U;

            ee24_write_protect(handle, GPIO_PIN_RESET);

            while ((err == EE24_ERR_NONE) && (done < len))
            {
                uint32_t      at     = address + (uint32_t)done;
                uint32_t      chunk  = ee24_chunk(at, len - done, page);
                ee24_target_t target = ee24_target(handle, at);
                uint32_t      left   = ee24_remaining(start, timeout_ms);

                /* The HAL takes a pointer to non-const, but a write only reads
                   through it, so casting the const away here is safe. */
                uint8_t *bytes = (uint8_t *)&data[done];

                if (left == 0U)
                {
                    err = EE24_ERR_TIMEOUT;
                }
                else if (HAL_I2C_Mem_Write(handle->hi2c, target.dev_address, target.mem_address,
                                           target.mem_size, bytes, (uint16_t)chunk, left)
                         != HAL_OK)
                {
                    err = EE24_ERR_I2C;
                }
                else
                {
                    /* The chip stores the page after the transfer ends, and does
                       not answer anything until it has. */
                    err = ee24_wait_ready(handle, target.dev_address, start, timeout_ms);
                    done += chunk;
                }
            }

            ee24_write_protect(handle, GPIO_PIN_SET);
            ee24_unlock(handle);
        }
    }

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
 * Doubles as the check on size_kbit, so a mistyped size such as 250 is refused
 * by ee24_init() rather than used.
 *
 * @param[in] size_kbit  Size in kilobits, from the part name.
 * @return The size in bytes, or 0 when size_kbit is not a 24xx size.
 */
static uint32_t ee24_size_bytes(uint16_t size_kbit)
{
    uint32_t bytes = 0U;

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
 * A chip that is sent more than a page wraps round to the start of the same
 * page and overwrites what it has just been given, with nothing reported. From
 * 24C128 up the real page is 64 or 128 bytes. 32 is used there as well, which
 * is always safe because those pages are whole multiples of it, only slower.
 *
 * @param[in] size_kbit  Size in kilobits, already checked.
 * @return The page size in bytes.
 */
static uint32_t ee24_page_size(uint16_t size_kbit)
{
    uint32_t page = 32U;

    if (size_kbit <= 2U)
    {
        page = 8U;
    }
    else if (size_kbit <= 16U)
    {
        page = 16U;
    }
    else
    {
        /* 32 bytes, from 24C32 up. */
    }

    return page;
}

/*****************************************************************************************************/
/**
 * @brief How many bytes one read may carry.
 *
 * Chips from 24C04 to 24C16 split their memory into 256 byte blocks, each at
 * its own I2C address, so a read stops at the end of a block and the next one
 * starts at the next address. The largest chips are bigger than one HAL
 * transfer can count. Everything else fits in one.
 *
 * @param[in] size_kbit  Size in kilobits, already checked.
 * @return The most bytes one read may carry, as a boundary it must not cross.
 */
static uint32_t ee24_read_size(uint16_t size_kbit)
{
    uint32_t limit = EE24_READ_LIMIT;

    if ((size_kbit >= 4U) && (size_kbit <= 16U))
    {
        limit = EE24_BLOCK_SIZE;
    }

    return limit;
}

/*****************************************************************************************************/
/**
 * @brief Check the arguments every read and write share.
 *
 * @param[in] handle   Handle to check.
 * @param[in] address  First byte of the transfer.
 * @param[in] data     The caller's buffer.
 * @param[in] len      Length of the transfer.
 * @return EE24_ERR_NONE when the transfer can go ahead, EE24_ERR_INVALID for a
 *         NULL pointer or a handle that is not ready, or EE24_ERR_RANGE when the
 *         transfer runs past the end of the chip.
 */
static ee24_err_t ee24_check(const ee24_t *handle, uint32_t address, const void *data, size_t len)
{
    ee24_err_t err = EE24_ERR_INVALID;

    assert_param(handle != NULL);
    assert_param(data != NULL);

    if ((handle != NULL) && (data != NULL) && (handle->ready != 0U))
    {
        uint32_t size = ee24_size_bytes(handle->size_kbit);

        /* Written as a subtraction, because address + len can wrap past zero
           and then look small enough. */
        if ((address > size) || (len > (size_t)(size - address)))
        {
            err = EE24_ERR_RANGE;
        }
        else
        {
            err = EE24_ERR_NONE;
        }
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief How much of what is left fits before the next boundary.
 *
 * @param[in] address   Where the transfer starts.
 * @param[in] left      Bytes still to go.
 * @param[in] boundary  A page or block size. The transfer must not cross a
 *                      multiple of it.
 * @return Bytes for this transfer, at least 1 when left is not 0.
 */
static uint32_t ee24_chunk(uint32_t address, size_t left, uint32_t boundary)
{
    uint32_t room  = boundary - (address % boundary);
    uint32_t chunk = room;

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
 * From 24C32 up the memory address goes out as two bytes. Below that it is one
 * byte, which reaches 256 bytes, and chips from 24C04 to 24C16 take the higher
 * bits in the I2C address, where A0 to A2 would otherwise be. Bit 8 of the
 * memory address lands in bit 1 of the 8 bit I2C address, and so on up.
 *
 * @param[in] handle   Handle of the chip.
 * @param[in] address  Memory address, already checked against the size.
 * @return Everything the HAL needs to reach that address.
 */
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address)
{
    ee24_target_t target;

    if (handle->size_kbit >= 32U)
    {
        target.dev_address = handle->dev_address;
        target.mem_address = (uint16_t)address;
        target.mem_size    = (uint16_t)I2C_MEMADD_SIZE_16BIT;
    }
    else
    {
        target.dev_address = (uint16_t)(handle->dev_address | ((address >> 7U) & 0x0EU));
        target.mem_address = (uint16_t)(address & 0xFFU);
        target.mem_size    = (uint16_t)I2C_MEMADD_SIZE_8BIT;
    }

    return target;
}

/*****************************************************************************************************/
/**
 * @brief Milliseconds left of a timeout that started at start.
 *
 * Each HAL call is given what is left rather than the whole timeout, so the
 * timeout holds for the call as a whole however many transfers it takes.
 *
 * @param[in] start       HAL_GetTick() when the call began.
 * @param[in] timeout_ms  The caller's timeout.
 * @return Milliseconds left, or 0 once the timeout has run out.
 */
static uint32_t ee24_remaining(uint32_t start, uint32_t timeout_ms)
{
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
 * A chip that is busy storing a page does not acknowledge its address, so it
 * is asked every EE24_POLL_MS until it does. That is the method the datasheets
 * give, and it is usually well under the 5 to 10 ms a fixed wait would have to
 * allow. It sleeps before the first question, since no chip stores a page in
 * under a millisecond.
 *
 * @param[in] handle       Handle of the chip.
 * @param[in] dev_address  The I2C address the page went to.
 * @param[in] start        HAL_GetTick() when the call began.
 * @param[in] timeout_ms   The caller's timeout, for the whole call.
 * @return EE24_ERR_NONE once the chip answers, or EE24_ERR_TIMEOUT.
 */
static ee24_err_t ee24_wait_ready(const ee24_t *handle, uint16_t dev_address, uint32_t start,
                                  uint32_t timeout_ms)
{
    ee24_err_t err  = EE24_ERR_TIMEOUT;
    uint32_t   left = 0U;

    do
    {
        ee24_sleep();

        left = ee24_remaining(start, timeout_ms);

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
    if (handle->wp_port != NULL)
    {
        HAL_GPIO_WritePin(handle->wp_port, handle->wp_pin, state);
    }
}

/*****************************************************************************************************/
/**
 * @brief Create the handle's mutex. Does nothing without an RTOS.
 *
 * With CMSIS-RTOS the mutex comes from the RTOS heap, so a heap that is too
 * small shows up here as EE24_ERR_MUTEX.
 *
 * @param[in,out] handle  Handle of the chip.
 * @return EE24_ERR_NONE, or EE24_ERR_MUTEX when the RTOS could not create it.
 */
static ee24_err_t ee24_mutex_create(ee24_t *handle)
{
    ee24_err_t err = EE24_ERR_NONE;

#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    handle->mutex = osMutexCreate(osMutex(ee24_mutex));

    if (handle->mutex == NULL)
    {
        err = EE24_ERR_MUTEX;
    }
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    handle->mutex = osMutexNew(&ee24_mutex_attr);

    if (handle->mutex == NULL)
    {
        err = EE24_ERR_MUTEX;
    }
#elif EE24_RTOS == EE24_RTOS_THREADX
    if (tx_mutex_create(&handle->mutex, (CHAR *)"ee24", TX_INHERIT) != TX_SUCCESS)
    {
        err = EE24_ERR_MUTEX;
    }
#else
    (void)handle;
#endif

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Take the handle's mutex, when the RTOS is running.
 *
 * Before the RTOS starts there is only one thread of execution, so there is
 * nothing to guard against, and blocking would not work yet anyway. That is
 * what lets a project read its settings from the chip in main() before it
 * starts the RTOS.
 *
 * @param[in,out] handle      Handle of the chip.
 * @param[in]     timeout_ms  How long to wait for another thread to finish.
 * @return EE24_ERR_NONE, EE24_ERR_TIMEOUT when another thread held it for too
 *         long, or EE24_ERR_MUTEX for any other refusal, such as a call from
 *         an interrupt.
 */
static ee24_err_t ee24_lock(ee24_t *handle, uint32_t timeout_ms)
{
    ee24_err_t err = EE24_ERR_NONE;

#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    if (ee24_kernel_running())
    {
        /* CMSIS-RTOS v1 waits in milliseconds, and its osWaitForever is the same
           value as HAL_MAX_DELAY, so the timeout goes in as it is. */
        osStatus status = osMutexWait(handle->mutex, timeout_ms);

        if ((status == osErrorTimeoutResource) || (status == osErrorResource))
        {
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != osOK)
        {
            err = EE24_ERR_MUTEX;
        }
        else
        {
            /* Taken. */
        }
    }
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    if (ee24_kernel_running())
    {
        uint32_t   ticks  = osWaitForever;
        osStatus_t status = osError;

        if (timeout_ms != HAL_MAX_DELAY)
        {
            ticks = ee24_ticks(timeout_ms, osKernelGetTickFreq());
        }

        status = osMutexAcquire(handle->mutex, ticks);

        if ((status == osErrorTimeout) || (status == osErrorResource))
        {
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != osOK)
        {
            err = EE24_ERR_MUTEX;
        }
        else
        {
            /* Taken. */
        }
    }
#elif EE24_RTOS == EE24_RTOS_THREADX
    if (ee24_kernel_running())
    {
        ULONG ticks  = TX_WAIT_FOREVER;
        UINT  status = TX_SUCCESS;

        if (timeout_ms != HAL_MAX_DELAY)
        {
            ticks = ee24_ticks(timeout_ms, (uint32_t)TX_TIMER_TICKS_PER_SECOND);
        }

        status = tx_mutex_get(&handle->mutex, ticks);

        if (status == TX_NOT_AVAILABLE)
        {
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != TX_SUCCESS)
        {
            err = EE24_ERR_MUTEX;
        }
        else
        {
            /* Taken. */
        }
    }
#else
    (void)handle;
    (void)timeout_ms;
#endif

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Give back what ee24_lock() took.
 *
 * Whether the RTOS is running cannot change between the two calls, since
 * starting it never returns to main(), so asking again gives the same answer
 * ee24_lock() acted on.
 *
 * @param[in,out] handle  Handle of the chip.
 */
static void ee24_unlock(ee24_t *handle)
{
#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    if (ee24_kernel_running())
    {
        (void)osMutexRelease(handle->mutex);
    }
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    if (ee24_kernel_running())
    {
        (void)osMutexRelease(handle->mutex);
    }
#elif EE24_RTOS == EE24_RTOS_THREADX
    if (ee24_kernel_running())
    {
        (void)tx_mutex_put(&handle->mutex);
    }
#else
    (void)handle;
#endif
}

/*****************************************************************************************************/
/**
 * @brief Wait EE24_POLL_MS, letting other threads run when there is an RTOS.
 *
 * An RTOS sleeps at least one tick, so with a tick slower than 1 ms the wait
 * is one tick instead.
 */
static void ee24_sleep(void)
{
#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    if (ee24_kernel_running())
    {
        /* CMSIS-RTOS v1 takes milliseconds. */
        (void)osDelay(EE24_POLL_MS);
    }
    else
    {
        HAL_Delay(EE24_POLL_MS);
    }
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    if (ee24_kernel_running())
    {
        (void)osDelay(ee24_ticks(EE24_POLL_MS, osKernelGetTickFreq()));
    }
    else
    {
        HAL_Delay(EE24_POLL_MS);
    }
#elif EE24_RTOS == EE24_RTOS_THREADX
    if (ee24_kernel_running())
    {
        (void)tx_thread_sleep(ee24_ticks(EE24_POLL_MS, (uint32_t)TX_TIMER_TICKS_PER_SECOND));
    }
    else
    {
        HAL_Delay(EE24_POLL_MS);
    }
#else
    HAL_Delay(EE24_POLL_MS);
#endif
}

#if EE24_RTOS != EE24_RTOS_NONE
/*****************************************************************************************************/
/**
 * @brief Whether the RTOS is running, so a thread may block.
 *
 * For ThreadX that means being called from a thread: before tx_kernel_enter()
 * there is no current thread, and a thread cannot sleep or wait any earlier.
 *
 * @return true once the RTOS is running.
 */
static bool ee24_kernel_running(void)
{
#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    /* -1 means FreeRTOS was built without a way to tell. Assume running, which
       is the safe side: the mutex is taken as it always was. */
    return osKernelRunning() != 0;
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    return osKernelGetState() == osKernelRunning;
#else
    return tx_thread_identify() != TX_NULL;
#endif
}
#endif

#if (EE24_RTOS == EE24_RTOS_CMSIS_V2) || (EE24_RTOS == EE24_RTOS_THREADX)
/*****************************************************************************************************/
/**
 * @brief Turn milliseconds into RTOS ticks, rounding up.
 *
 * Rounding up keeps a short wait from becoming no wait at all on a slow tick.
 * The result stops one short of the "wait forever" value, so a long finite
 * timeout cannot turn into an endless one.
 *
 * @param[in] ms       Milliseconds.
 * @param[in] tick_hz  The RTOS tick rate.
 * @return The number of ticks.
 */
static uint32_t ee24_ticks(uint32_t ms, uint32_t tick_hz)
{
    uint64_t ticks = (((uint64_t)ms * tick_hz) + 999U) / 1000U;

    if (ticks > EE24_TICKS_MAX)
    {
        ticks = EE24_TICKS_MAX;
    }

    return (uint32_t)ticks;
}
#endif
