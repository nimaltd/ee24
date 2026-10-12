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

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

/* How often ee24_wait() asks whether the job has moved on. */
#define EE24_POLL_MS            1U

/* How hard ee24_init() tries to find the chip. */
#define EE24_INIT_TRIALS        2U
#define EE24_INIT_TIMEOUT_MS    100U

/* EE24_TIMEOUT_TRANSFER_MS is for every this many bytes a transfer carries. */
#define EE24_TRANSFER_UNIT      256U

/* The HAL counts a transfer in a uint16_t, so a 24C512, 64 KB, cannot be read
   in one. Reads are split at every 32 KB instead. */
#define EE24_READ_LIMIT         0x8000U

/* One address byte reaches 256 bytes. Chips from 24C04 to 24C16 take the rest
   in the I2C address, so one transfer must not cross from one block to the next. */
#define EE24_BLOCK_SIZE         256U

/* What a job is doing. */
#define EE24_JOB_IDLE           0U
#define EE24_JOB_READ           1U  /* DMA bringing data in. */
#define EE24_JOB_SEND           2U  /* DMA sending a page.   */
#define EE24_JOB_STORE          3U  /* The chip storing it.  */

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
static ee24_err_t    ee24_page_start(ee24_t *handle);
#if (EE24_TRANSFER == EE24_TRANSFER_DMA)
static ee24_err_t    ee24_piece_start(ee24_t *handle);
static ee24_err_t    ee24_job_moved(ee24_t *handle);
#endif
static ee24_err_t    ee24_job_stored(ee24_t *handle);
static void          ee24_job_end(ee24_t *handle, ee24_err_t err);
static ee24_err_t    ee24_range(const ee24_t *handle, uint32_t address, size_t len);
static uint32_t      ee24_chunk(uint32_t address, size_t left, uint32_t boundary);
static ee24_target_t ee24_target(const ee24_t *handle, uint32_t address);
static uint32_t      ee24_transfer_time(uint32_t bytes);
static uint32_t      ee24_remaining(uint32_t start, uint32_t timeout_ms);
static ee24_err_t    ee24_i2c_error(const ee24_t *handle, HAL_StatusTypeDef status);
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

        /* No job runs on a new handle. Left as it was, a handle full of
           rubbish would refuse every call as busy. */
        handle->job_state  = EE24_JOB_IDLE;
        handle->job_result = EE24_ERR_NONE;

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
 * By polling it returns with the data read. With EE24_TRANSFER_DMA it is a job:
 * it returns once the read has started, and ee24_is_busy() or ee24_wait() says
 * when it has finished. Until then the data is not there yet, and the buffer
 * must stay where it is.
 *
 * @param[in,out] handle   Handle from ee24_init().
 * @param[in]     address  First byte to read.
 * @param[out]    data     Where the bytes go. With DMA, in memory the DMA can reach.
 * @param[in]     len      Bytes to read, up to the end of the chip.
 * @return EE24_ERR_NONE, or the EE24_ERR_ value that says what went wrong.
 */
ee24_err_t ee24_read(ee24_t *handle, uint32_t address, uint8_t *data, size_t len)
{
    ee24_err_t err  = EE24_ERR_INVALID;
    osal_err_t lock = OSAL_ERR_NONE;
#if (EE24_TRANSFER == EE24_TRANSFER_POLLING)
    size_t     done = 0U;
#endif

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

        /* One job at a time. ee24_is_busy() says when the last has ended. */
        if (handle->job_state != EE24_JOB_IDLE)
        {
            err = EE24_ERR_BUSY;
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
        lock = osal_mutex_lock(&handle->mutex, EE24_TIMEOUT_MUTEX_MS);

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

#if (EE24_TRANSFER == EE24_TRANSFER_DMA)
        /* No break from here on: the mutex is held until the job ends. The DMA
           brings the data in a piece at a time, and ee24_is_busy() starts each
           piece after the first. */
        handle->job_state   = EE24_JOB_READ;
        handle->job_data    = data;
        handle->job_len     = len;
        handle->job_done    = 0U;
        handle->job_address = address;
        err                 = ee24_piece_start(handle);

        if (err != EE24_ERR_NONE)
        {
            ee24_job_end(handle, err);
        }
#else
        /* No break from here on: the mutex is held, and is given back below.
           Read in pieces: 24C04 to 24C16 put each 256 byte block at its own
           I2C address, and the HAL cannot count a whole 24C512 at once. */
        while ((err == EE24_ERR_NONE) && (done < len))
        {
            uint32_t          at     = address + (uint32_t)done;
            uint32_t          chunk  = ee24_chunk(at, len - done, handle->read_size);
            ee24_target_t     target = ee24_target(handle, at);
            HAL_StatusTypeDef status = HAL_OK;

            /* Each piece gets the time its size needs, so a 32 KB piece gets
               128 times what a short one gets. */
            status = HAL_I2C_Mem_Read(handle->hi2c, target.dev_address, target.mem_address,
                                      target.mem_size, &data[done], (uint16_t)chunk,
                                      ee24_transfer_time(chunk));
            err    = ee24_i2c_error(handle, status);
            done  += chunk;
        }

        /* Give the chip back to other threads, error or not. */
        osal_mutex_unlock(&handle->mutex);
#endif
    }
    while (false);

    /* What ee24_wait() reports. A job started here, or one still running,
       puts its own result over this when it ends. */
    handle->job_result = err;

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Start writing len bytes starting at address. Not from an interrupt.
 *
 * A job: it returns once the first page has started, and ee24_is_busy() sends
 * each of the others once the chip has stored the one before. ee24_is_busy()
 * or ee24_wait() says when the last is stored. Until then the buffer must stay
 * as it is.
 *
 * @param[in,out] handle   Handle from ee24_init().
 * @param[in]     address  First byte to write.
 * @param[in]     data     The bytes to write. With DMA, in memory the DMA can reach.
 * @param[in]     len      Bytes to write, up to the end of the chip.
 * @return EE24_ERR_NONE once started, or the EE24_ERR_ value that says what
 *         went wrong. After an error, the pages before the failing one are written.
 */
ee24_err_t ee24_write(ee24_t *handle, uint32_t address, const uint8_t *data, size_t len)
{
    ee24_err_t err  = EE24_ERR_INVALID;
    osal_err_t lock = OSAL_ERR_NONE;

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

        /* One job at a time. ee24_is_busy() says when the last has ended. */
        if (handle->job_state != EE24_JOB_IDLE)
        {
            err = EE24_ERR_BUSY;
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
        lock = osal_mutex_lock(&handle->mutex, EE24_TIMEOUT_MUTEX_MS);

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

        /* No break from here on: the mutex is held and write protect is off
           until the job ends. A page at a time: sent more than a page, the
           chip wraps round inside it and overwrites what it has just been
           given. The HAL takes a pointer to non-const, but sending only reads
           through it, so casting the const away is safe. */
        ee24_write_protect(handle, GPIO_PIN_RESET);

        handle->job_data    = (uint8_t *)data;
        handle->job_len     = len;
        handle->job_done    = 0U;
        handle->job_address = address;
        err                 = ee24_page_start(handle);

        if (err != EE24_ERR_NONE)
        {
            ee24_job_end(handle, err);
        }
    }
    while (false);

    /* What ee24_wait() reports. A job started here, or one still running,
       puts its own result over this when it ends. */
    handle->job_result = err;

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Whether a job is still running. Moves it on, and returns at once. Not from an interrupt.
 *
 * A job is a write, or with DMA a read too. Each call checks once: it asks the
 * chip, or the HAL with DMA, whether the piece under way has finished, and if
 * it has, starts the next page or piece. Call it from the thread that started
 * the job, which holds the mutex until the job ends. Once it returns false,
 * ee24_wait() says how the job ended.
 *
 * @param[in,out] handle  Handle from ee24_init().
 * @return true while the job runs, false once it has ended or when none runs.
 */
bool ee24_is_busy(ee24_t *handle)
{
    ee24_err_t err = EE24_ERR_NONE;

    assert_param(handle != NULL);

    if (handle->job_state == EE24_JOB_STORE)
    {
        /* The chip is storing a page: ask it once whether it has finished. */
        err = ee24_job_stored(handle);
    }
#if (EE24_TRANSFER == EE24_TRANSFER_DMA)
    else if (handle->job_state != EE24_JOB_IDLE)
    {
        /* Data is moving: ask the HAL once whether the DMA has finished. */
        err = ee24_job_moved(handle);
    }
#endif
    else
    {
        /* Nothing runs. */
    }

    /* Ended, well or not: the chip and the mutex are given back, and how it
       ended is kept for ee24_wait(). */
    if ((err != EE24_ERR_BUSY) && (handle->job_state != EE24_JOB_IDLE))
    {
        ee24_job_end(handle, err);
    }

    return (err == EE24_ERR_BUSY);
}

/*****************************************************************************************************/
/**
 * @brief Wait until the running job has ended, and say how it ended. Not from an interrupt.
 *
 * Asks first, so after ee24_is_busy() has returned false, or after a call that
 * finished or was refused at once, it returns straight away. Between the
 * questions it sleeps a millisecond through osal, so with an RTOS other
 * threads run. Call it from the thread that started the job.
 *
 * @param[in,out] handle  Handle from ee24_init().
 * @return How the last read or write ended: EE24_ERR_NONE when it worked.
 */
ee24_err_t ee24_wait(ee24_t *handle)
{
    assert_param(handle != NULL);

    while (ee24_is_busy(handle))
    {
        osal_delay_ms(EE24_POLL_MS);
    }

    return handle->job_result;
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
 * @brief Send the next page of a write job, by polling or DMA.
 *
 * @param[in,out] handle  Handle of the chip, with a write job under way.
 * @return EE24_ERR_NONE once sent, or the EE24_ERR_ value that says what went wrong.
 */
static ee24_err_t ee24_page_start(ee24_t *handle)
{
    uint32_t          at     = handle->job_address + (uint32_t)handle->job_done;
    ee24_target_t     target = ee24_target(handle, at);
    HAL_StatusTypeDef status = HAL_OK;

    /* From here to the end of the page, or less when that is all there is. */
    handle->job_step    = ee24_chunk(at, handle->job_len - handle->job_done, handle->page_size);
    handle->job_start   = HAL_GetTick();
    handle->job_timeout = ee24_transfer_time(handle->job_step);

#if (EE24_TRANSFER == EE24_TRANSFER_DMA)
    /* The DMA sends the page, and ee24_is_busy() sees it go. */
    handle->job_state = EE24_JOB_SEND;
    status            = HAL_I2C_Mem_Write_DMA(handle->hi2c, target.dev_address, target.mem_address,
                                              target.mem_size, &handle->job_data[handle->job_done],
                                              (uint16_t)handle->job_step);
#else
    status = HAL_I2C_Mem_Write(handle->hi2c, target.dev_address, target.mem_address,
                               target.mem_size, &handle->job_data[handle->job_done],
                               (uint16_t)handle->job_step, handle->job_timeout);

    /* The chip stores the page once the transfer ends, and does not answer
       anything until it has. Its time starts now. */
    handle->job_state   = EE24_JOB_STORE;
    handle->job_start   = HAL_GetTick();
    handle->job_timeout = EE24_TIMEOUT_PAGE_MS;
#endif

    return ee24_i2c_error(handle, status);
}

#if (EE24_TRANSFER == EE24_TRANSFER_DMA)

/*****************************************************************************************************/
/**
 * @brief Start the DMA on the next piece of a read job.
 *
 * @param[in,out] handle  Handle of the chip, with a read job under way.
 * @return EE24_ERR_NONE once started, or the EE24_ERR_ value that says what went wrong.
 */
static ee24_err_t ee24_piece_start(ee24_t *handle)
{
    uint32_t          at     = handle->job_address + (uint32_t)handle->job_done;
    ee24_target_t     target = ee24_target(handle, at);
    HAL_StatusTypeDef status = HAL_OK;

    /* To the end of the block, or 32 KB, or less when that is all there is.
       Each piece is a transfer of its own, with its own address. */
    handle->job_step    = ee24_chunk(at, handle->job_len - handle->job_done, handle->read_size);
    handle->job_start   = HAL_GetTick();
    handle->job_timeout = ee24_transfer_time(handle->job_step);

    status = HAL_I2C_Mem_Read_DMA(handle->hi2c, target.dev_address, target.mem_address,
                                  target.mem_size, &handle->job_data[handle->job_done],
                                  (uint16_t)handle->job_step);

    return ee24_i2c_error(handle, status);
}

/*****************************************************************************************************/
/**
 * @brief Ask the HAL once whether the DMA has finished, and go on when it has.
 *
 * @param[in,out] handle  Handle of the chip, with data moving.
 * @return EE24_ERR_BUSY while the job runs, EE24_ERR_NONE when a read is done,
 *         or what went wrong.
 */
static ee24_err_t ee24_job_moved(ee24_t *handle)
{
    ee24_err_t err = EE24_ERR_BUSY;

    if (HAL_I2C_GetState(handle->hi2c) != HAL_I2C_STATE_READY)
    {
        if (ee24_remaining(handle->job_start, handle->job_timeout) == 0U)
        {
            /* Stuck: the bus held low, or the DMA or the I2C interrupt not
               running. The HAL has no way to stop a memory transfer part way,
               so the I2C is set up again from scratch, which CubeMX's
               HAL_I2C_MspInit() does with its DMA. Left as it is, the HAL
               would refuse every later transfer as busy. */
            (void)HAL_I2C_DeInit(handle->hi2c);
            (void)HAL_I2C_Init(handle->hi2c);
            err = EE24_ERR_TIMEOUT;
        }
    }
    else if (HAL_I2C_GetError(handle->hi2c) != HAL_I2C_ERROR_NONE)
    {
        /* Finished is not the same as finished well: no acknowledge, or a
           DMA error. */
        err = EE24_ERR_I2C;
    }
    else if (handle->job_state == EE24_JOB_SEND)
    {
        /* The page is out. The chip stores it now, and its time starts. */
        handle->job_state   = EE24_JOB_STORE;
        handle->job_start   = HAL_GetTick();
        handle->job_timeout = EE24_TIMEOUT_PAGE_MS;
    }
    else
    {
        /* A piece of a read is in: on to the next, if there is one. */
        handle->job_done += handle->job_step;

        if (handle->job_done < handle->job_len)
        {
            err = ee24_piece_start(handle);
            err = (err == EE24_ERR_NONE) ? EE24_ERR_BUSY : err;
        }
        else
        {
            err = EE24_ERR_NONE;
        }
    }

    return err;
}

#endif /* EE24_TRANSFER == EE24_TRANSFER_DMA */

/*****************************************************************************************************/
/**
 * @brief Ask the chip once whether it has stored the page, and send the next one if so.
 *
 * @param[in,out] handle  Handle of the chip, storing a page.
 * @return EE24_ERR_BUSY while the job runs, EE24_ERR_NONE when it is done, or
 *         what went wrong.
 */
static ee24_err_t ee24_job_stored(ee24_t *handle)
{
    uint32_t      at     = handle->job_address + (uint32_t)handle->job_done;
    ee24_target_t target = ee24_target(handle, at);
    ee24_err_t    err    = EE24_ERR_BUSY;

    /* A chip busy storing a page does not acknowledge its address, so an
       answer means it has finished. This is the method the datasheets give,
       and it is usually well under the 5 to 10 ms a fixed wait would have to
       allow. The question is one byte long. */
    if (HAL_I2C_IsDeviceReady(handle->hi2c, target.dev_address, 1U, ee24_transfer_time(1U))
        == HAL_OK)
    {
        /* Stored: on to the next page, if there is one. */
        handle->job_done += handle->job_step;

        if (handle->job_done < handle->job_len)
        {
            err = ee24_page_start(handle);
            err = (err == EE24_ERR_NONE) ? EE24_ERR_BUSY : err;
        }
        else
        {
            err = EE24_ERR_NONE;
        }
    }
    else if (ee24_remaining(handle->job_start, handle->job_timeout) == 0U)
    {
        /* Still storing, long past what any chip needs. */
        err = EE24_ERR_TIMEOUT;
    }
    else
    {
        /* Still storing. */
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief End a job: protect the chip again, give the mutex back, and keep how it ended.
 *
 * @param[in,out] handle  Handle of the chip.
 * @param[in]     err     How the job ended.
 */
static void ee24_job_end(ee24_t *handle, ee24_err_t err)
{
    /* A write lifted write protect for the job, on every way out, errors
       included. A read never touched it. */
    if (handle->job_state != EE24_JOB_READ)
    {
        ee24_write_protect(handle, GPIO_PIN_SET);
    }

    handle->job_state  = EE24_JOB_IDLE;
    handle->job_result = err;
    osal_mutex_unlock(&handle->mutex);
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
 * @brief The time a transfer of this many bytes may take.
 *
 * @param[in] bytes  Bytes the transfer carries.
 * @return Milliseconds, or HAL_MAX_DELAY to wait for ever.
 */
static uint32_t ee24_transfer_time(uint32_t bytes)
{
    uint32_t time = HAL_MAX_DELAY;

#if EE24_TIMEOUT_TRANSFER_MS != HAL_MAX_DELAY
    /* The setting is for every 256 bytes, rounded up, so a page gets it once
       and a 32 KB read 128 times. */
    time = ((bytes + (EE24_TRANSFER_UNIT - 1U)) / EE24_TRANSFER_UNIT) * EE24_TIMEOUT_TRANSFER_MS;
#else
    /* For ever stays for ever. Multiplied, it would wrap round to a short time. */
    (void)bytes;
#endif

    return time;
}

/*****************************************************************************************************/
/**
 * @brief Milliseconds left of a timeout.
 *
 * @param[in] start       HAL_GetTick() when the step began.
 * @param[in] timeout_ms  The time the step may take.
 * @return Milliseconds left, or 0 once it has run out.
 */
static uint32_t ee24_remaining(uint32_t start, uint32_t timeout_ms)
{
    /* A subtraction, which stays right when the tick wraps after 49 days. */
    uint32_t elapsed = HAL_GetTick() - start;
    uint32_t left    = 0U;

    if (timeout_ms == HAL_MAX_DELAY)
    {
        /* For ever never runs out. */
        left = HAL_MAX_DELAY;
    }
    else if (elapsed < timeout_ms)
    {
        left = timeout_ms - elapsed;
    }
    else
    {
        /* Run out. */
    }

    return left;
}

/*****************************************************************************************************/
/**
 * @brief Turn what the HAL returned into an ee24 error.
 *
 * @param[in] handle  Handle of the chip.
 * @param[in] status  What the HAL returned.
 * @return EE24_ERR_NONE, EE24_ERR_TIMEOUT or EE24_ERR_I2C.
 */
static ee24_err_t ee24_i2c_error(const ee24_t *handle, HAL_StatusTypeDef status)
{
    ee24_err_t err = EE24_ERR_NONE;

    if (status != HAL_OK)
    {
        /* A transfer that ran out of time comes back as HAL_ERROR on some
           families and HAL_TIMEOUT on others, but the HAL notes it in the
           error code on all of them. */
        err = ((HAL_I2C_GetError(handle->hi2c) & HAL_I2C_ERROR_TIMEOUT) != 0U) ? EE24_ERR_TIMEOUT
                                                                               : EE24_ERR_I2C;
    }

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
