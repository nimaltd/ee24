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

static uint32_t      ee24_size_bytes(uint16_t size_kbit);
static uint32_t      ee24_page_size(uint16_t size_kbit);
static uint32_t      ee24_read_size(uint16_t size_kbit);
static ee24_err_t    ee24_check(const ee24_t *handle, uint32_t address, const void *data,
                                size_t len);
static uint32_t      ee24_chunk(uint32_t address, size_t left, uint32_t boundary);
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address);
static uint32_t      ee24_remaining(uint32_t start, uint32_t timeout_ms);
static ee24_err_t    ee24_wait_ready(const ee24_t *handle, uint16_t dev_address, uint32_t start,
                                     uint32_t timeout_ms);
static void          ee24_write_protect(const ee24_t *handle, GPIO_PinState state);
static ee24_err_t    ee24_mutex_create(ee24_t *handle);
static ee24_err_t    ee24_lock(ee24_t *handle, uint32_t timeout_ms);
static void          ee24_unlock(ee24_t *handle);
static void          ee24_sleep(void);
#if EE24_RTOS != EE24_RTOS_NONE
static bool          ee24_kernel_running(void);
#endif
#if (EE24_RTOS == EE24_RTOS_CMSIS_V2) || (EE24_RTOS == EE24_RTOS_THREADX)
static uint32_t      ee24_ticks(uint32_t ms, uint32_t tick_hz);
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
    ee24_err_t err = EE24_ERR_INVALID;

    assert_param(handle != NULL);
    assert_param(hi2c != NULL);

    if (handle != NULL)
    {
        /* Refused until every step below has worked, so a handle whose setup
           failed half way is never used. */
        handle->ready = 0U;

        /* A size that is not a 24xx part is refused here, before anything
           is sent to the bus. */
        if ((hi2c != NULL) && (ee24_size_bytes(size_kbit) != 0U))
        {
            handle->hi2c        = hi2c;
            handle->dev_address = dev_address;
            handle->size_kbit   = size_kbit;
            handle->wp_port     = wp_port;
            handle->wp_pin      = wp_pin;

            /* Protected from the start. A write releases it only while it runs. */
            ee24_write_protect(handle, GPIO_PIN_SET);

            /* Does a chip answer at this address? */
            if (HAL_I2C_IsDeviceReady(hi2c, dev_address, EE24_INIT_TRIALS, EE24_INIT_TIMEOUT_MS)
                != HAL_OK)
            {
                err = EE24_ERR_I2C;
            }
            else
            {
                /* The mutex comes last, so a chip that did not answer leaves no
                   mutex behind, and calling init again makes only one. */
                err = ee24_mutex_create(handle);
            }

            /* Only now may ee24_read() and ee24_write() use it. */
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
       timeout as well. Arguments are checked before anything else. */
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = ee24_check(handle, address, data, len);

    if ((err == EE24_ERR_NONE) && (len > 0U))
    {
        /* One thread at a time. Without an RTOS this does nothing. */
        err = ee24_lock(handle, timeout_ms);

        if (err == EE24_ERR_NONE)
        {
            /* Read in pieces: 24C04 to 24C16 put each 256 byte block at its own
               I2C address, and the HAL cannot count a whole 24C512 at once. */
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
                    /* Out of time before the next piece. */
                    err = EE24_ERR_TIMEOUT;
                }
                else if (HAL_I2C_Mem_Read(handle->hi2c, target.dev_address, target.mem_address,
                                          target.mem_size, &data[done], (uint16_t)chunk, left)
                         != HAL_OK)
                {
                    /* The HAL gets only what is left of the timeout, so the
                       call as a whole keeps to it. */
                    err = EE24_ERR_I2C;
                }
                else
                {
                    done += chunk;
                }
            }

            /* Give the chip back to other threads, error or not. */
            ee24_unlock(handle);
        }
    }

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
       timeout as well. Arguments are checked before anything else. */
    uint32_t   start = HAL_GetTick();
    ee24_err_t err   = ee24_check(handle, address, data, len);

    if ((err == EE24_ERR_NONE) && (len > 0U))
    {
        /* One thread at a time. Without an RTOS this does nothing. */
        err = ee24_lock(handle, timeout_ms);

        if (err == EE24_ERR_NONE)
        {
            /* Write a page at a time. Sent more than a page, the chip wraps
               round inside it and overwrites what it has just been given. */
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
                    /* Out of time before the next page. */
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
                       not answer anything until it has. So the next page, or
                       the caller's next call, waits for it here. */
                    err = ee24_wait_ready(handle, target.dev_address, start, timeout_ms);
                    done += chunk;
                }
            }

            /* Protected again and given back on every way out, errors included. */
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
static uint32_t ee24_page_size(uint16_t size_kbit)
{
    uint32_t page = 32U;

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
static uint32_t ee24_read_size(uint16_t size_kbit)
{
    /* The HAL counts a transfer in 16 bits, so a 24C512, 64 KB, is read in
       two halves. Every smaller chip fits in one. */
    uint32_t limit = EE24_READ_LIMIT;

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
 * @brief Check the arguments every read and write share.
 *
 * @param[in] handle   Handle to check.
 * @param[in] address  First byte of the transfer.
 * @param[in] data     The caller's buffer.
 * @param[in] len      Length of the transfer.
 * @return EE24_ERR_NONE, EE24_ERR_INVALID or EE24_ERR_RANGE.
 */
static ee24_err_t ee24_check(const ee24_t *handle, uint32_t address, const void *data, size_t len)
{
    ee24_err_t err = EE24_ERR_INVALID;

    assert_param(handle != NULL);
    assert_param(data != NULL);

    /* A handle ee24_init() did not accept is refused like a NULL one. */
    if ((handle != NULL) && (data != NULL) && (handle->ready != 0U))
    {
        uint32_t size = ee24_size_bytes(handle->size_kbit);

        /* Written as a subtraction, because address + len can wrap past zero
           and then look small enough. Sent anyway, the chip would wrap round
           and overwrite its own start. */
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

    if (handle->size_kbit >= 32U)
    {
        /* From 24C32 up the memory address goes out as two bytes. */
        target.dev_address = handle->dev_address;
        target.mem_address = (uint16_t)address;
        target.mem_size    = (uint16_t)I2C_MEMADD_SIZE_16BIT;
    }
    else
    {
        /* Below that it is one byte, which reaches 256 bytes. 24C04 to 24C16
           take the higher bits in the I2C address, where A0 to A2 would be:
           bit 8 of the memory address lands in bit 1 of the 8 bit I2C address,
           and so on up. On a 24C01 or 24C02 those bits are always 0. */
        target.dev_address = (uint16_t)(handle->dev_address | ((address >> 7U) & 0x0EU));
        target.mem_address = (uint16_t)(address & 0xFFU);
        target.mem_size    = (uint16_t)I2C_MEMADD_SIZE_8BIT;
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
           asking straight away would only waste a question. */
        ee24_sleep();

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

/*****************************************************************************************************/
/**
 * @brief Create the handle's mutex. Does nothing without an RTOS.
 *
 * @param[in,out] handle  Handle of the chip.
 * @return EE24_ERR_NONE or EE24_ERR_MUTEX.
 */
static ee24_err_t ee24_mutex_create(ee24_t *handle)
{
    ee24_err_t err = EE24_ERR_NONE;

#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    /* From the RTOS heap, so NULL most often means the heap is too small. */
    handle->mutex = osMutexCreate(osMutex(ee24_mutex));

    if (handle->mutex == NULL)
    {
        err = EE24_ERR_MUTEX;
    }
#elif EE24_RTOS == EE24_RTOS_CMSIS_V2
    /* From the RTOS heap, so NULL most often means the heap is too small. */
    handle->mutex = osMutexNew(&ee24_mutex_attr);

    if (handle->mutex == NULL)
    {
        err = EE24_ERR_MUTEX;
    }
#elif EE24_RTOS == EE24_RTOS_THREADX
    /* Inside the handle, so no heap is needed. Priority inheritance, so a low
       priority thread in the middle of a write is not held up by a medium one
       while a high priority thread waits for the chip. */
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
 * @param[in,out] handle      Handle of the chip.
 * @param[in]     timeout_ms  How long to wait for another thread.
 * @return EE24_ERR_NONE, EE24_ERR_TIMEOUT or EE24_ERR_MUTEX.
 */
static ee24_err_t ee24_lock(ee24_t *handle, uint32_t timeout_ms)
{
    ee24_err_t err = EE24_ERR_NONE;

    /* Before the RTOS starts there is only main(), so nothing to guard
       against, and blocking would not work yet anyway. That is what lets a
       project read its settings from the chip before it starts the RTOS. */
#if EE24_RTOS == EE24_RTOS_CMSIS_V1
    if (ee24_kernel_running())
    {
        /* CMSIS-RTOS v1 waits in milliseconds, and its osWaitForever is the same
           value as HAL_MAX_DELAY, so the timeout goes in as it is. */
        osStatus status = osMutexWait(handle->mutex, timeout_ms);

        if ((status == osErrorTimeoutResource) || (status == osErrorResource))
        {
            /* Another thread kept it for the whole wait. */
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != osOK)
        {
            /* Refused outright, such as from an interrupt. */
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

        /* HAL_MAX_DELAY waits for ever. Anything else becomes ticks. */
        if (timeout_ms != HAL_MAX_DELAY)
        {
            ticks = ee24_ticks(timeout_ms, osKernelGetTickFreq());
        }

        status = osMutexAcquire(handle->mutex, ticks);

        if ((status == osErrorTimeout) || (status == osErrorResource))
        {
            /* Another thread kept it for the whole wait. */
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != osOK)
        {
            /* Refused outright, such as from an interrupt. */
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

        /* HAL_MAX_DELAY waits for ever. Anything else becomes ticks. */
        if (timeout_ms != HAL_MAX_DELAY)
        {
            ticks = ee24_ticks(timeout_ms, (uint32_t)TX_TIMER_TICKS_PER_SECOND);
        }

        status = tx_mutex_get(&handle->mutex, ticks);

        if (status == TX_NOT_AVAILABLE)
        {
            /* Another thread kept it for the whole wait. */
            err = EE24_ERR_TIMEOUT;
        }
        else if (status != TX_SUCCESS)
        {
            /* Refused outright, such as from an interrupt. */
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
 * @param[in,out] handle  Handle of the chip.
 */
static void ee24_unlock(ee24_t *handle)
{
    /* The same test ee24_lock() made, and the answer cannot have changed in
       between, since starting the RTOS never returns to main(). So this gives
       back exactly what was taken, and nothing when nothing was. */
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
 */
static void ee24_sleep(void)
{
    /* With the RTOS running the thread sleeps, so other threads run, at least
       one tick even on a tick slower than 1 ms. Before it starts, and without
       one, the HAL tick is all there is. */
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
    /* ThreadX has no other way to ask. Before tx_kernel_enter() there is no
       current thread, and nothing may sleep or wait any earlier. */
    return tx_thread_identify() != TX_NULL;
#endif
}
#endif

#if (EE24_RTOS == EE24_RTOS_CMSIS_V2) || (EE24_RTOS == EE24_RTOS_THREADX)
/*****************************************************************************************************/
/**
 * @brief Turn milliseconds into RTOS ticks, rounding up.
 *
 * @param[in] ms       Milliseconds.
 * @param[in] tick_hz  The RTOS tick rate.
 * @return The number of ticks.
 */
static uint32_t ee24_ticks(uint32_t ms, uint32_t tick_hz)
{
    /* Rounded up, so a short wait does not become no wait at all on a slow
       tick. 64 bits, so a long wait cannot overflow on the way. */
    uint64_t ticks = (((uint64_t)ms * tick_hz) + 999U) / 1000U;

    /* One short of the RTOS's "wait for ever", so a long finite timeout
       cannot turn into an endless one. */
    if (ticks > EE24_TICKS_MAX)
    {
        ticks = EE24_TICKS_MAX;
    }

    return (uint32_t)ticks;
}
#endif
