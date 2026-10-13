/**
 * @file        test_ee24.c
 * @brief       Host unit tests for the ee24 library, built on Unity.
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
 *
 * @note        Runs on a PC, not on hardware. The HAL calls land on a model of
 *              a 24xx chip that follows the datasheets: a write wraps inside
 *              its page, a busy chip does not answer, WP high blocks writes,
 *              and chips from 24C04 to 24C16 take their block from the I2C
 *              address. Time is a clock the tests move, so a write cycle is
 *              checked instantly, and a DMA transfer finishes once the clock
 *              has passed its end. osal is a fake the tests control, so a
 *              mutex held by another thread or refused by the RTOS is tested
 *              here too, with no RTOS at all. The suite is built once by
 *              polling, once with DMA, and once with every time HAL_MAX_DELAY.
 */

/*
 * ****************************************************************************************************
 * Includes
 * ****************************************************************************************************
*/

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "ee24.h"

/*
 * ****************************************************************************************************
 * Macros
 * ****************************************************************************************************
*/

/* Which build this is. */
#define USES_DMA            (EE24_TRANSFER == EE24_TRANSFER_DMA)
#define FOREVER             (EE24_TIMEOUT_TRANSFER_MS == HAL_MAX_DELAY)

/* The most one read transfer carries in this build: 32 KB with DMA, and by
   polling no more than EE24_POLL_CHUNK either. */
#define READ_PIECE          (USES_DMA ? 32768U : ((EE24_POLL_CHUNK < 32768U) ? EE24_POLL_CHUNK : 32768U))

/* Big enough for the largest chip, a 24C512. */
#define CHIP_MAX_BYTES      65536U

/* How long the modelled chip takes to store a page. Datasheets give 5 ms as the
   most, and a real part is usually quicker. */
#define WRITE_CYCLE_US      3500U

/* One byte on the bus at 400 kHz, near enough: 9 clocks. */
#define BYTE_US             25U

/* The same at 100 kHz, the slowest clock the chips are made for. */
#define BYTE_US_100K        90U

/* How many transfers of each kind the model remembers the details of. */
#define RECORD_MAX          64U

#define WP_PIN              GPIO_PIN_5

/*
 * ****************************************************************************************************
 * Types
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief A 24xx chip on the bus, the bus itself, and what happened to them.
 */
typedef struct
{
    uint8_t     mem[CHIP_MAX_BYTES];          /**< The memory, 0xFF when new.            */
    uint32_t    size;                         /**< Bytes.                                */
    uint32_t    page;                         /**< The real page size of such a part.    */
    uint16_t    mem_size;                     /**< Address bytes it expects, HAL form.   */
    uint16_t    base_address;                 /**< I2C address, 8 bit form.              */
    bool        present;                      /**< Whether it is on the bus at all.      */
    bool        wp_high;                      /**< What the WP pin is driven to.         */
    uint32_t    busy_until_us;                /**< When the current write cycle ends.    */
    uint32_t    write_cycle_us;               /**< How long a page takes to store.       */
    uint32_t    byte_us;                      /**< One byte on the bus, at its clock.    */
    uint32_t    i2c_error;                    /**< What HAL_I2C_GetError() reports.      */
    int         fail_write_at;                /**< Write transfer that fails, 0 for none. */
    int         fail_read_at;                 /**< Read transfer that fails, 0 for none. */
    bool        dma_running;                  /**< A DMA transfer has not been seen end. */
    bool        dma_stall;                    /**< DMA transfers never finish.           */
    bool        i2c_down;                     /**< HAL_I2C_DeInit() ran, Init not yet.   */
    uint32_t    dma_done_us;                  /**< When the running one finishes.        */
    uint32_t    dma_error;                    /**< An error every DMA transfer ends in.  */
    uint32_t    dma_result;                   /**< What the running one ends in.         */
    int         dma_refuse_at;                /**< Read the HAL will not start, 0 none.  */
    int         transfers;                    /**< Every transfer of any kind.           */
    int         writes;                       /**< Write transfers.                      */
    int         reads;                        /**< Read transfers.                       */
    int         polls;                        /**< Is it ready? questions.               */
    int         dma_transfers;                /**< Transfers by DMA.                     */
    int         i2c_resets;                   /**< HAL_I2C_Init() after a DeInit.        */
    int         writes_ignored;               /**< Writes sent while WP was high.        */
    int         faults;                       /**< Things a real chip would get wrong.   */
    const char  *fault;                       /**< What the first of them was.           */
    uint16_t    last_dev_address;             /**< I2C address of the last transfer.     */
    uint32_t    poll_timeout;                 /**< Timeout given with the last question. */
    uint16_t    write_sizes[RECORD_MAX];      /**< Length of each write transfer.        */
    uint32_t    write_timeouts[RECORD_MAX];   /**< Timeout given with each one.          */
    uint16_t    read_sizes[RECORD_MAX];       /**< Length of each read transfer.         */
    uint32_t    read_timeouts[RECORD_MAX];    /**< Timeout given with each one.          */
    uint32_t    poll_times_us[RECORD_MAX];    /**< When each question was asked.         */

} fake_chip_t;

/*
 * ****************************************************************************************************
 * Global variables
 * ****************************************************************************************************
*/

static fake_chip_t       chip;
static ee24_t            ee;
static I2C_HandleTypeDef test_i2c;
static GPIO_TypeDef      test_wp_port;

static uint32_t          now_us       = 0U;
static uint32_t          tick_jump_ms = 0U;
static int               gpio_writes  = 0;

static uint8_t           pattern[CHIP_MAX_BYTES];
static uint8_t           readback[CHIP_MAX_BYTES];

/* assert_param, as CubeMX builds it with Enable Full Assert. */
static jmp_buf assert_return;
static bool    assert_expected = false;
static int     asserts         = 0;

/* The fake osal. */
static osal_err_t         lock_result        = OSAL_ERR_NONE;
static bool               mutex_create_fails = false;
static const osal_mutex_t *mutex_created_at  = NULL;
static int                mutex_creates      = 0;
static int                mutex_takes        = 0;
static int                mutex_gives        = 0;
static int                mutex_held         = 0;
static uint32_t           mutex_last_wait    = 0U;
static int                sleeps             = 0;
static uint32_t           last_sleep_ms      = 0U;

/*
 * ****************************************************************************************************
 * Private function prototypes
 * ****************************************************************************************************
*/

static void       chip_reset(uint16_t size_kbit);
static bool       assert_stops(int which);
static void       counts_clear(void);
static void       chip_fault(const char *what);
static void       transfer_begins(const I2C_HandleTypeDef *hi2c);
static bool       bus_time(uint32_t bytes, uint32_t timeout);
static void       dma_begins(uint32_t bytes);
static void       write_begins(uint16_t dev_address, uint16_t size, uint32_t timeout);
static void       read_begins(uint16_t dev_address, uint16_t size, uint32_t timeout);
static bool       chip_answers(void);
static bool       chip_store(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                             const uint8_t *data, uint16_t size, uint32_t end_us);
static bool       chip_load(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                            uint8_t *data, uint16_t size);
static uint16_t   chip_block_bits(void);
static bool       chip_addressed(uint16_t dev_address);
static bool       chip_decode(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                              const uint8_t *data, uint16_t size, uint32_t *address);
static ee24_err_t init_chip(uint16_t size_kbit, bool with_wp);
static ee24_err_t init_chip_at(uint16_t size_kbit, uint16_t chip_address, uint8_t dev_address);
static ee24_err_t settle(ee24_err_t err);
static ee24_err_t read_all(uint32_t address, uint8_t *data, size_t len);
static ee24_err_t write_all(uint32_t address, const uint8_t *data, size_t len);
static void       pattern_fill(uint32_t seed);

/*
 * ****************************************************************************************************
 * Public function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Where assert_param lands with USE_FULL_ASSERT, as the user's main.c defines it.
 *
 * A test that expects it jumps back to assert_stops(). Anywhere else a valid
 * call tripped an assert, which fails the test.
 *
 * @param[in] file  Source file of the assert.
 * @param[in] line  Its line.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;

    asserts++;

    if (!assert_expected)
    {
        TEST_FAIL_MESSAGE("assert_param stopped a valid call");
    }

    longjmp(assert_return, 1);
}

/*****************************************************************************************************/
/**
 * @brief Return the tick, from the tests' clock.
 *
 * now_us reaches only 71 minutes, so a test that needs the tick far on, past
 * its wrap after 49 days, moves it with tick_jump_ms instead.
 *
 * @return Milliseconds since the clock was reset.
 */
uint32_t HAL_GetTick(void)
{
    return (now_us / 1000U) + tick_jump_ms;
}

/*****************************************************************************************************/
/**
 * @brief Wait, by moving the clock on. ee24 has to sleep through osal instead.
 *
 * @param[in] Delay  Milliseconds.
 */
void HAL_Delay(uint32_t Delay)
{
    /* With an RTOS this spins, and keeps every other thread waiting. */
    chip_fault("waited in HAL_Delay() and not through osal");

    now_us += Delay * 1000U;
}

/*****************************************************************************************************/
/**
 * @brief Write to the modelled chip, by polling.
 *
 * The data wraps inside its page, the way a real chip stores it. A write sent
 * while WP is high is acknowledged and then ignored, which is also what a real
 * chip does, so a write protect pin left high shows up as data that never
 * arrived. A transfer the bus is too slow for gives up at its timeout.
 *
 * @return HAL_OK when the chip took the data, HAL_ERROR otherwise.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                    uint16_t Size, uint32_t Timeout)
{
    HAL_StatusTypeDef status = HAL_ERROR;

    if (USES_DMA)
    {
        chip_fault("wrote by polling with EE24_TRANSFER_DMA");
    }

    write_begins(DevAddress, Size, Timeout);
    transfer_begins(hi2c);

    if (!bus_time(3U + Size, Timeout))
    {
        /* Out of time part way, which the HAL notes as a timeout. */
    }
    else if (chip_store(DevAddress, MemAddress, MemAddSize, pData, Size, now_us))
    {
        status = HAL_OK;
    }
    else
    {
        chip.i2c_error = HAL_I2C_ERROR_AF;
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Read from the modelled chip, by polling.
 *
 * @return HAL_OK when the chip answered, HAL_ERROR otherwise.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout)
{
    HAL_StatusTypeDef status = HAL_ERROR;

    if (USES_DMA)
    {
        chip_fault("read by polling with EE24_TRANSFER_DMA");
    }

    read_begins(DevAddress, Size, Timeout);
    transfer_begins(hi2c);

    if (!bus_time(4U + Size, Timeout))
    {
        /* Out of time part way, which the HAL notes as a timeout. */
    }
    else if (chip_load(DevAddress, MemAddress, MemAddSize, pData, Size))
    {
        status = HAL_OK;
    }
    else
    {
        chip.i2c_error = HAL_I2C_ERROR_AF;
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Write to the modelled chip by DMA, finishing later on the tests' clock.
 *
 * The chip takes the data now, and its write cycle starts when the transfer
 * ends. A chip that does not answer shows up when the transfer ends, as it
 * does on the HAL that sends the address by interrupt.
 *
 * @return HAL_OK once started, HAL_BUSY while another runs.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Write_DMA(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                        uint16_t Size)
{
    HAL_StatusTypeDef status = HAL_BUSY;

    if (!USES_DMA)
    {
        chip_fault("wrote by DMA with EE24_TRANSFER_POLLING");
    }

    transfer_begins(hi2c);

    if (!chip.dma_running)
    {
        write_begins(DevAddress, Size, 0U);
        dma_begins(3U + Size);

        if (!chip.dma_stall && (chip.dma_result == HAL_I2C_ERROR_NONE) &&
            !chip_store(DevAddress, MemAddress, MemAddSize, pData, Size, chip.dma_done_us))
        {
            chip.dma_result = HAL_I2C_ERROR_AF;
        }

        status = HAL_OK;
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Read from the modelled chip by DMA, finishing later on the tests' clock.
 *
 * @return HAL_OK once started, HAL_BUSY while another runs.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Read_DMA(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                       uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                       uint16_t Size)
{
    HAL_StatusTypeDef status = HAL_BUSY;

    if (!USES_DMA)
    {
        chip_fault("read by DMA with EE24_TRANSFER_POLLING");
    }

    transfer_begins(hi2c);

    if (chip.reads + 1 == chip.dma_refuse_at)
    {
        /* The HAL will not start this one, and says so at once. */
        chip.reads++;
        status = HAL_ERROR;
    }
    else if (!chip.dma_running)
    {
        read_begins(DevAddress, Size, 0U);
        dma_begins(4U + Size);

        if (!chip.dma_stall && (chip.dma_result == HAL_I2C_ERROR_NONE) &&
            !chip_load(DevAddress, MemAddress, MemAddSize, pData, Size))
        {
            chip.dma_result = HAL_I2C_ERROR_AF;
        }

        status = HAL_OK;
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Whether a DMA transfer is still running. The first look after its end ends it.
 *
 * @return HAL_I2C_STATE_BUSY while it runs, HAL_I2C_STATE_READY otherwise.
 */
HAL_I2C_StateTypeDef HAL_I2C_GetState(I2C_HandleTypeDef *hi2c)
{
    HAL_I2C_StateTypeDef state = HAL_I2C_STATE_READY;

    if (hi2c != &test_i2c)
    {
        chip_fault("asked the state of the wrong I2C handle");
    }

    if (chip.dma_running)
    {
        if (chip.dma_stall || (now_us < chip.dma_done_us))
        {
            state = HAL_I2C_STATE_BUSY;
        }
        else
        {
            chip.dma_running = false;
            chip.i2c_error   = chip.dma_result;
        }
    }

    return state;
}

/*****************************************************************************************************/
/**
 * @brief The error the last transfer ended with.
 *
 * @return A HAL_I2C_ERROR_ value.
 */
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &test_i2c)
    {
        chip_fault("asked the error of the wrong I2C handle");
    }

    return chip.i2c_error;
}

/*****************************************************************************************************/
/**
 * @brief Take the I2C down, which stops a DMA transfer.
 *
 * @return HAL_OK.
 */
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c != &test_i2c)
    {
        chip_fault("took down the wrong I2C handle");
    }

    chip.dma_running = false;
    chip.i2c_error   = HAL_I2C_ERROR_NONE;
    chip.i2c_down    = true;

    return HAL_OK;
}

/*****************************************************************************************************/
/**
 * @brief Set the I2C up again.
 *
 * @return HAL_OK.
 */
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c != &test_i2c) || !chip.i2c_down)
    {
        chip_fault("set an I2C up that was not taken down");
    }

    chip.i2c_down = false;
    chip.i2c_resets++;

    return HAL_OK;
}

/*****************************************************************************************************/
/**
 * @brief Ask the modelled chip whether it answers.
 *
 * @return HAL_OK when it acknowledged its address, HAL_ERROR when it did not.
 */
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout)
{
    HAL_StatusTypeDef status = HAL_ERROR;
    uint32_t          trial  = 0U;

    chip.poll_timeout = Timeout;

    for (trial = 0U; (trial < Trials) && (status != HAL_OK); trial++)
    {
        if (chip.polls < (int)RECORD_MAX)
        {
            chip.poll_times_us[chip.polls] = now_us;
        }

        chip.polls++;
        transfer_begins(hi2c);
        (void)bus_time(1U, Timeout);

        if (chip_answers() && chip_addressed(DevAddress))
        {
            status = HAL_OK;
        }
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Set a pin. Only the write protect pin may be touched.
 */
void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    gpio_writes++;

    if ((GPIOx != &test_wp_port) || (GPIO_Pin != WP_PIN))
    {
        chip_fault("a pin other than write protect was driven");
    }
    else
    {
        chip.wp_high = (PinState == GPIO_PIN_SET);
    }
}

/*****************************************************************************************************/
/**
 * @brief Create the fake mutex, unless the test says it cannot be made.
 *
 * @param[out] mutex  Mutex to create.
 * @return OSAL_ERR_NONE, or OSAL_ERR_MUTEX when the test says the heap is full.
 */
osal_err_t osal_mutex_create(osal_mutex_t *mutex)
{
    osal_err_t err = OSAL_ERR_MUTEX;

    mutex_creates++;

    if (!mutex_create_fails)
    {
        mutex_created_at = mutex;
        err              = OSAL_ERR_NONE;
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Take the fake mutex, keeping count, and give the answer the test chose.
 *
 * @param[in,out] mutex       Mutex to take.
 * @param[in]     timeout_ms  The wait it was given.
 * @return The answer the test chose.
 */
osal_err_t osal_mutex_lock(osal_mutex_t *mutex, uint32_t timeout_ms)
{
    if ((mutex == NULL) || (mutex != mutex_created_at))
    {
        chip_fault("asked for a mutex init never made");
    }

    mutex_takes++;
    mutex_last_wait = timeout_ms;

    if (lock_result == OSAL_ERR_NONE)
    {
        mutex_held++;
    }

    return lock_result;
}

/*****************************************************************************************************/
/**
 * @brief Give the fake mutex back, keeping count.
 *
 * @param[in,out] mutex  Mutex to give back.
 */
void osal_mutex_unlock(osal_mutex_t *mutex)
{
    if ((mutex == NULL) || (mutex != mutex_created_at))
    {
        chip_fault("gave back a mutex init never made");
    }

    if (mutex_held == 0)
    {
        chip_fault("gave back a mutex it did not hold");
    }
    else
    {
        mutex_held--;
    }

    mutex_gives++;
}

/*****************************************************************************************************/
/**
 * @brief Sleep, by moving the clock on.
 *
 * @param[in] ms  Milliseconds.
 */
void osal_delay_ms(uint32_t ms)
{
    sleeps++;
    last_sleep_ms = ms;
    now_us += ms * 1000U;
}

/*****************************************************************************************************/
/**
 * @brief Start every test from a new 24C256, a stopped clock and a free mutex.
 */
void setUp(void)
{
    now_us             = 0U;
    tick_jump_ms       = 0U;
    lock_result        = OSAL_ERR_NONE;
    mutex_create_fails = false;
    mutex_created_at   = NULL;
    mutex_creates      = 0;

    assert_expected = false;
    asserts         = 0;

    memset(&ee, 0, sizeof(ee));
    pattern_fill(0U);
    chip_reset(256U);
    counts_clear();
}

/*****************************************************************************************************/
/**
 * @brief Required by Unity. Nothing to undo after a test.
 */
void tearDown(void)
{
}

/*****************************************************************************************************/
/**
 * @brief Every 24xx size from 24C01 to 24C512 is accepted.
 */
void test_every_size_is_accepted(void)
{
    static const uint16_t sizes[] = { 1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U };
    size_t                i       = 0U;

    for (i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++)
    {
        TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, init_chip(sizes[i], false),
                                      "a real size was refused");
    }
}

/*****************************************************************************************************/
/**
 * @brief A size that is not a 24xx part is refused before the bus is touched.
 */
void test_a_size_that_does_not_exist_is_refused(void)
{
    static const uint16_t sizes[] = { 0U, 3U, 24U, 250U, 1024U, 65535U };
    size_t                i       = 0U;

    for (i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++)
    {
        TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID,
                              ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, sizes[i], NULL, 0U));
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.transfers, "the bus was used for a bad size");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U),
                                  "a handle with a bad size was used");
}

/*****************************************************************************************************/
/**
 * @brief A NULL pointer is stopped by assert_param before anything is touched.
 *
 * The library does not test pointers again: a NULL is the caller's bug, and
 * a debug build with Enable Full Assert stops at the line that found it.
 */
void test_null_pointers_are_caught_by_assert(void)
{
    int which = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    for (which = 0; which < 8; which++)
    {
        TEST_ASSERT_TRUE_MESSAGE(assert_stops(which), "a NULL got past assert_param");
    }

    TEST_ASSERT_EQUAL_INT(8, asserts);
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_takes, "a NULL call took the mutex");
}

/*****************************************************************************************************/
/**
 * @brief A chip that does not answer is reported, and its handle is not used.
 */
void test_a_chip_that_does_not_answer_is_reported(void)
{
    chip.present = false;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));

    counts_clear();

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_write(&ee, 0U, pattern, 1U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.transfers, "a handle that failed init was used");
}

/*****************************************************************************************************/
/**
 * @brief A handle that worked is refused after a later init of it fails.
 *
 * Otherwise a second init with a mistyped size would leave the handle half
 * changed and still in use.
 */
void test_a_failed_init_leaves_the_handle_refused(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 250U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.present = false;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U));
}

/*****************************************************************************************************/
/**
 * @brief A chip at another address does not answer, so init says so.
 */
void test_the_wrong_address_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_init(&ee, &test_i2c, 0xA8U, 256U, NULL, 0U));
}

/*****************************************************************************************************/
/**
 * @brief Init can simply be called again once the chip answers.
 */
void test_init_can_be_retried(void)
{
    chip.present = false;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));

    chip.present = true;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 4U));
}

/*****************************************************************************************************/
/**
 * @brief A handle full of rubbish works once ee24_init() has run.
 *
 * Version 3 never cleared its lock flag in init, so a handle declared as a
 * local variable could start out locked and hang the first call for good.
 * Here the rubbish would also say a job was running.
 */
void test_a_handle_full_of_rubbish_works_after_init(void)
{
    ee24_t local;

    memset(&local, 0xA5, sizeof(local));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE,
                          ee24_init(&local, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));

    /* Straight in, before anything else has looked at the handle. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, ee24_write(&local, 10U, pattern, 8U),
                                  "a job left over from the rubbish refused the write");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&local));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&local, 10U, readback, 8U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&local));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 8U);
    TEST_ASSERT_FALSE(ee24_is_running(&local));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief The last byte of the chip can be written and read.
 */
void test_the_last_byte_is_in_range(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(chip.size - 1U, pattern, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(chip.size - 1U, readback, 1U));
    TEST_ASSERT_EQUAL_UINT8(pattern[0], readback[0]);
}

/*****************************************************************************************************/
/**
 * @brief Anything that runs past the end of the chip is refused, with no traffic.
 *
 * Version 3 sent it anyway, and the chip wrapped round and overwrote its start.
 */
void test_past_the_end_is_refused(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size - 1U, pattern, 2U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size, pattern, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, chip.size - 1U, readback, 2U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 0U, readback, chip.size + 1U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.transfers, "the bus was used for a bad range");
}

/*****************************************************************************************************/
/**
 * @brief A range whose end wraps past zero is still refused.
 */
void test_a_range_that_wraps_is_refused(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 0xFFFFFFFFU, readback, 2U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 16U, readback, SIZE_MAX));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, 16U, pattern, SIZE_MAX - 8U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
}

/*****************************************************************************************************/
/**
 * @brief Nothing to do is done at once, and succeeds.
 */
void test_zero_bytes_does_nothing(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 0U));
    TEST_ASSERT_FALSE_MESSAGE(ee24_is_running(&ee), "nothing to do started a job");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 0U));
    TEST_ASSERT_FALSE(ee24_is_running(&ee));

    /* Nothing, just past the last byte, still fits. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, chip.size, readback, 0U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_takes, "nothing to do still took the mutex");
}

/*****************************************************************************************************/
/**
 * @brief Every size stores what it is given, at the right place, and reads it back.
 *
 * The data starts at an odd address and runs to the end, so it crosses every
 * page and every block. It is compared with the chip's memory directly as
 * well as read back, because a read and a write that make the same addressing
 * mistake would still agree with each other.
 */
void test_every_size_reads_back_what_was_written(void)
{
    static const uint16_t sizes[] = { 1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U };
    size_t                i       = 0U;

    for (i = 0U; i < (sizeof(sizes) / sizeof(sizes[0])); i++)
    {
        uint32_t len = 0U;

        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(sizes[i], false));

        len = chip.size - 3U;
        pattern_fill(sizes[i]);
        memset(readback, 0, sizeof(readback));

        TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, write_all(3U, pattern, len),
                                      "the write failed");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, &chip.mem[3], len,
                                         "the chip holds something else");
        TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, read_all(3U, readback, len),
                                      "the read failed");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, readback, len, "read back something else");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
    }
}

/*****************************************************************************************************/
/**
 * @brief A whole 24C512 reads in one call.
 *
 * 64 KB is one more than the HAL can count in a transfer. Version 3 handed it
 * over in one, and the HAL got a length of 0 and refused.
 */
void test_a_whole_24c512_reads_in_one_call(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 65536U));
    TEST_ASSERT_EQUAL_INT((int)(65536U / READ_PIECE), chip.reads);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief A write that starts inside a page is split where the pages end.
 */
void test_a_write_is_split_at_every_page(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(32U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(30U, pattern, 40U));

    TEST_ASSERT_EQUAL_INT(3, chip.writes);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2U, chip.write_sizes[0], "did not stop at the first page end");
    TEST_ASSERT_EQUAL_UINT16(32U, chip.write_sizes[1]);
    TEST_ASSERT_EQUAL_UINT16(6U, chip.write_sizes[2]);
}

/*****************************************************************************************************/
/**
 * @brief A read on a 24C16 stops at the end of each 256 byte block.
 *
 * With pieces of 256 or more that is 56, 256, 256 and 32 bytes. Smaller
 * pieces split it further, but none crosses a block either.
 */
void test_a_read_is_split_at_every_block(void)
{
    uint32_t at     = 200U;
    uint32_t left   = 600U;
    int      pieces = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(16U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(200U, readback, 600U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);

    /* Each piece runs to the end of its block, or to the end of the read, and
       is no bigger than this build reads at once. */
    while (left > 0U)
    {
        uint32_t piece = 256U - (at % 256U);

        piece = (piece > left) ? left : piece;
        piece = (piece > READ_PIECE) ? READ_PIECE : piece;

        TEST_ASSERT_EQUAL_UINT16(piece, chip.read_sizes[pieces]);

        at   += piece;
        left -= piece;
        pieces++;
    }

    TEST_ASSERT_EQUAL_INT(pieces, chip.reads);
    TEST_ASSERT_EQUAL_INT_MESSAGE((READ_PIECE >= 256U) ? 4 : pieces, chip.reads,
                                  "a block was split into more pieces than it needed");
}

/*****************************************************************************************************/
/**
 * @brief Chips from 24C04 to 24C16 find each block at its own I2C address.
 */
void test_each_block_has_its_own_address(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(16U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0x7FFU, pattern, 1U));
    TEST_ASSERT_EQUAL_HEX16(0xAEU, chip.last_dev_address);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(4U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0x100U, readback, 1U));
    TEST_ASSERT_EQUAL_HEX16(0xA2U, chip.last_dev_address);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(2U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0xFFU, readback, 1U));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xA0U, chip.last_dev_address, "a 24C02 has only one block");
}

/*****************************************************************************************************/
/**
 * @brief Each EE24_ADDRESS_ value is the address its pins give, and reaches a chip wired that way.
 */
void test_every_address_reaches_its_chip(void)
{
    /* A0 is bit 1 of the 8 bit address, A1 bit 2 and A2 bit 3. */
    static const uint8_t addresses[] = {
        EE24_ADDRESS_DEFAULT, EE24_ADDRESS_A0,    EE24_ADDRESS_A1,    EE24_ADDRESS_A0_A1,
        EE24_ADDRESS_A2,      EE24_ADDRESS_A0_A2, EE24_ADDRESS_A1_A2, EE24_ADDRESS_A0_A1_A2,
    };
    static const uint8_t expected[] = { 0xA0U, 0xA2U, 0xA4U, 0xA6U, 0xA8U, 0xAAU, 0xACU, 0xAEU };
    size_t               i          = 0U;

    for (i = 0U; i < (sizeof(addresses) / sizeof(addresses[0])); i++)
    {
        TEST_ASSERT_EQUAL_HEX8(expected[i], addresses[i]);

        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip_at(256U, expected[i], addresses[i]));
        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(100U, pattern, 4U));
        TEST_ASSERT_EQUAL_MEMORY(pattern, &chip.mem[100], 4U);
        TEST_ASSERT_EQUAL_HEX16(expected[i], chip.last_dev_address);
    }
}

/*****************************************************************************************************/
/**
 * @brief The pins a 24C04 to 24C16 ignores are ignored, so its first block stays reachable.
 *
 * Such a chip puts its block number where those pins would be. Kept in the
 * address, a pin tied high would send the first block to the second.
 */
void test_the_pins_a_small_chip_ignores_are_ignored(void)
{
    /* A0 on a 24C04, A0 and A1 on a 24C08, all three on a 24C16. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip_at(4U, 0xA0U, EE24_ADDRESS_A0));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 4U));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, &chip.mem[0], 4U, "the first block was missed");
    TEST_ASSERT_EQUAL_HEX16(0xA0U, chip.last_dev_address);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip_at(8U, 0xA0U, EE24_ADDRESS_A0_A1));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 4U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, &chip.mem[0], 4U);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip_at(16U, 0xA0U, EE24_ADDRESS_A0_A1_A2));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 4U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, &chip.mem[0], 4U);

    /* A1 still counts on a 24C04, so two of them can share a bus. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip_at(4U, 0xA4U, EE24_ADDRESS_A0_A1));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0x100U, pattern, 4U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, &chip.mem[0x100], 4U);
    TEST_ASSERT_EQUAL_HEX16(0xA6U, chip.last_dev_address);

    /* Every pin counts on a 24C02 and from 24C32 up, so a chip with all of
       them grounded does not answer to A0. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, init_chip_at(2U, 0xA0U, EE24_ADDRESS_A0));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, init_chip_at(32U, 0xA0U, EE24_ADDRESS_A0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief A write waits for the chip, not for a fixed 10 ms a page.
 *
 * Ten pages of a chip that stores each in 3.5 ms. Version 3 waited 10 ms after
 * each one, over 100 ms in all.
 */
void test_a_write_waits_only_as_long_as_the_chip_needs(void)
{
    uint32_t start = 0U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    start = HAL_GetTick();
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 320U));

    TEST_ASSERT_EQUAL_INT(10, chip.writes);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(15U, 50U, HAL_GetTick() - start,
                                      "the write took far longer or shorter than the chip needed");
}

/*****************************************************************************************************/
/**
 * @brief ee24_wait() asks the chip once a millisecond, sleeping between, and never sooner.
 */
void test_the_chip_is_asked_every_millisecond(void)
{
    int i = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 96U));

    /* Three pages, each stored in 3.5 ms, so each is asked a few times. Asking
       less often would wait longer than it needs. */
    TEST_ASSERT_TRUE_MESSAGE(chip.polls >= 9, "not asked once a millisecond");
    TEST_ASSERT_TRUE_MESSAGE(chip.polls <= (sleeps + 1), "asked without sleeping between");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, last_sleep_ms, "slept more than a millisecond");

    for (i = 1; (i < chip.polls) && (i < (int)RECORD_MAX); i++)
    {
        TEST_ASSERT_TRUE_MESSAGE((chip.poll_times_us[i] - chip.poll_times_us[i - 1]) >= 1000U,
                                 "asked again in under a millisecond");
    }
}

/*****************************************************************************************************/
/**
 * @brief The chip has finished storing when ee24_wait() returns.
 */
void test_the_chip_is_ready_when_the_wait_returns(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 5U));

    TEST_ASSERT_TRUE_MESSAGE(chip.busy_until_us <= now_us, "returned while the chip was busy");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 5U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 5U);
}

/*****************************************************************************************************/
/**
 * @brief A chip that never finishes a page ends in a timeout, after the page time.
 */
void test_a_chip_that_never_finishes_times_out(void)
{
    uint32_t start = 0U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.write_cycle_us = 0xF0000000U;

    start = HAL_GetTick();
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, write_all(0U, pattern, 1U));
    TEST_ASSERT_UINT32_WITHIN(2U, EE24_TIMEOUT_PAGE_MS + 1U, HAL_GetTick() - start);
    TEST_ASSERT_FALSE(ee24_is_running(&ee));

    /* Followed by hand: the one call that sees the time run out says the job
       is over, and ee24_wait() then says how. The chip answers again first,
       so it takes the page, and then never finishes that one either. */
    chip.busy_until_us = now_us;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 1U));

    /* Past the page's transfer, so with DMA too the chip is now storing it. */
    now_us += 2000U;
    TEST_ASSERT_TRUE(ee24_is_running(&ee));
    now_us += (EE24_TIMEOUT_PAGE_MS + 3U) * 1000U;
    TEST_ASSERT_FALSE_MESSAGE(ee24_is_running(&ee), "said running on the call that ended the job");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_wait(&ee));
}

/*****************************************************************************************************/
/**
 * @brief The page time is for each page, so a long write of a slow chip still finishes.
 */
void test_the_page_time_is_for_each_page(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    /* 9 ms a page is inside the 15 ms each page gets, though the ten of them
       take over 90 ms. */
    chip.write_cycle_us = 9000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 320U));
    TEST_ASSERT_EQUAL_INT(10, chip.writes);
    TEST_ASSERT_TRUE(HAL_GetTick() > 90U);

    /* At 100 kHz a page takes over 3 ms on the wire, and a chip that then
       needs 13.5 ms still has its 15: the time starts once the page has gone. */
    counts_clear();
    chip.byte_us        = BYTE_US_100K;
    chip.write_cycle_us = 13500U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, write_all(0U, pattern, 64U),
                                  "the page time counted the page's own transfer");
    chip.byte_us = BYTE_US;

    /* 20 ms a page is not, and the job stops at the first page. */
    counts_clear();
    chip.write_cycle_us = 20000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, write_all(0U, pattern, 320U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, chip.writes, "kept writing after a page timed out");
}

/*****************************************************************************************************/
/**
 * @brief A bus too slow for its transfer times ends in a timeout, and the I2C is free again.
 *
 * At 2 ms a byte, 256 bytes take half a second, far past the 35 ms the
 * setting gives them.
 */
void test_a_bus_too_slow_times_out(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.byte_us = 2000U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, read_all(0U, readback, 256U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(USES_DMA ? 1 : 0, chip.i2c_resets,
                                  "a stuck DMA transfer was not stopped, or a polling one was");
    TEST_ASSERT_FALSE(ee24_is_running(&ee));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_held, "kept the mutex after a timeout");

    /* Back at 400 kHz it works again. */
    chip.byte_us = BYTE_US;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 256U));
}

/*****************************************************************************************************/
/**
 * @brief At 100 kHz, the slowest clock, a whole 24C512 reads within its time.
 *
 * Each 32 KB half takes about 3 s on the wire, and gets 128 times 35 ms.
 */
void test_a_slow_bus_reads_a_whole_chip(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));
    chip.byte_us = BYTE_US_100K;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 65536U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 64U));
}

/*****************************************************************************************************/
/**
 * @brief A failed write is reported, and nothing more is sent after it.
 */
void test_a_failed_write_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.fail_write_at = 2;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 320U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, chip.writes, "kept writing after a failure");
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
}

/*****************************************************************************************************/
/**
 * @brief A failed read is reported, and nothing more is asked after it.
 */
void test_a_failed_read_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));
    chip.fail_read_at = 1;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(0U, readback, 65536U));
    TEST_ASSERT_EQUAL_INT(1, chip.reads);
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
}

/*****************************************************************************************************/
/**
 * @brief Write protect is lifted only while a write runs, and the data lands.
 */
void test_write_protect_is_lifted_only_while_writing(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "init left the chip unprotected");

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 100U, pattern, 70U));
    TEST_ASSERT_FALSE_MESSAGE(chip.wp_high, "protected again while the job ran");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.writes_ignored, "wrote while still protected");
    TEST_ASSERT_EQUAL_MEMORY(pattern, &chip.mem[100], 70U);
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "left the chip unprotected");
}

/*****************************************************************************************************/
/**
 * @brief Write protect comes back after a failure and after a timeout.
 */
void test_write_protect_comes_back_after_an_error(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));

    chip.fail_write_at = 2;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 96U));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a failed write");

    chip.fail_write_at = 1;
    chip.writes        = 0;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 96U));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a first page that failed");

    chip.fail_write_at  = 0;
    chip.write_cycle_us = 0xF0000000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, write_all(0U, pattern, 1U));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a timeout");
}

/*****************************************************************************************************/
/**
 * @brief Without a write protect pin, no pin is ever touched.
 */
void test_no_pin_is_touched_without_write_protect(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 40U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 40U));

    TEST_ASSERT_EQUAL_INT(0, gpio_writes);
}

/*****************************************************************************************************/
/**
 * @brief A read leaves write protect alone, failed or not.
 */
void test_a_read_leaves_write_protect_alone(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 40U));

    chip.fail_read_at = chip.reads + 1;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(0U, readback, 40U));

    TEST_ASSERT_EQUAL_INT(0, gpio_writes);
}

/*****************************************************************************************************/
/**
 * @brief Init makes one mutex, and only for a chip that answers.
 */
void test_init_makes_one_mutex_for_a_chip_that_answers(void)
{
    chip.present = false;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_creates, "made a mutex for a chip that did not answer");

    chip.present = true;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(1, mutex_creates);
}

/*****************************************************************************************************/
/**
 * @brief A mutex the RTOS cannot make is reported, and the handle is not used.
 */
void test_a_mutex_that_cannot_be_made_is_reported(void)
{
    mutex_create_fails = true;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_MUTEX,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));

    counts_clear();

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U));
    TEST_ASSERT_EQUAL_INT(0, mutex_takes);
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
}

/*****************************************************************************************************/
/**
 * @brief Every call takes the mutex once and gives it back, on every way out.
 */
void test_every_call_gives_the_mutex_back(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 40U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 40U));

    chip.fail_write_at = chip.writes + 1;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 40U));

    chip.fail_read_at = chip.reads + 1;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(0U, readback, 40U));

    chip.fail_write_at  = 0;
    chip.write_cycle_us = 0xF0000000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, write_all(0U, pattern, 1U));

    TEST_ASSERT_EQUAL_INT(5, mutex_takes);
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, mutex_gives, "the mutex was kept after a call");
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief A call refused for its arguments never takes the mutex.
 */
void test_a_bad_argument_takes_no_mutex(void)
{
    ee24_t blank;

    memset(&blank, 0, sizeof(blank));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size, pattern, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&blank, 0U, readback, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 0U));

    TEST_ASSERT_EQUAL_INT(0, mutex_takes);
}

/*****************************************************************************************************/
/**
 * @brief A chip another thread keeps too long ends in a timeout, without using the bus.
 */
void test_a_mutex_held_elsewhere_times_out(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    lock_result = OSAL_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 4U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_read(&ee, 0U, readback, 4U));
    TEST_ASSERT_FALSE_MESSAGE(ee24_is_running(&ee), "a call that never had the chip left a job");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.transfers, "used the bus without the mutex");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_gives, "gave back a mutex it never had");
}

/*****************************************************************************************************/
/**
 * @brief A mutex the RTOS refuses, as from an interrupt, is reported as such.
 */
void test_a_refused_mutex_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    lock_result = OSAL_ERR_MUTEX;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_MUTEX, ee24_read(&ee, 0U, readback, 4U));

    /* osal answers this only for a NULL mutex, which ee24 never passes, but
       it must not read as success either. */
    lock_result = OSAL_ERR_INVALID;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_MUTEX, ee24_write(&ee, 0U, pattern, 4U));

    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_gives, "gave back a mutex it never had");
}

/*****************************************************************************************************/
/**
 * @brief The wait for the mutex is the one ee24_config.h sets, for a read and for a write.
 */
void test_the_mutex_wait_is_the_configured_one(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 4U));
    TEST_ASSERT_EQUAL_UINT32(EE24_TIMEOUT_MUTEX_MS, mutex_last_wait);

    /* A write takes the mutex in its own place, so it is checked on its own. */
    mutex_last_wait = 0U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 4U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(EE24_TIMEOUT_MUTEX_MS, mutex_last_wait,
                                     "a write did not wait as long as configured");
}

/*****************************************************************************************************/
/**
 * @brief ee24_wait() sleeps through osal, a millisecond at a time.
 */
void test_the_wait_sleeps_through_osal(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 40U));

    TEST_ASSERT_TRUE(sleeps > 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, last_sleep_ms, "a poll should sleep one millisecond");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief With nothing running, ee24_is_running() says no, and ee24_wait() returns at once.
 */
void test_nothing_started_is_not_running(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_FALSE(ee24_is_running(&ee));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));
    TEST_ASSERT_EQUAL_INT(0, sleeps);
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
}

/*****************************************************************************************************/
/**
 * @brief A write returns once its first page has started, and ee24_is_running() sends the others.
 */
void test_a_write_returns_after_its_first_page(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 96U));
    TEST_ASSERT_EQUAL_INT(1, chip.writes);
    TEST_ASSERT_EQUAL_INT(0, sleeps);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mutex_held, "gave the mutex back while writing");

    /* The caller's state machine does other work between its questions. */
    while (ee24_is_running(&ee))
    {
        now_us += 100U;
    }

    TEST_ASSERT_EQUAL_INT(3, chip.writes);
    TEST_ASSERT_EQUAL_MEMORY(pattern, chip.mem, 96U);
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sleeps, "ee24_is_running() or ee24_wait() slept");
}

/*****************************************************************************************************/
/**
 * @brief While a job runs every other call is refused as busy, with no traffic, and the job goes on.
 */
void test_a_call_while_a_job_runs_is_refused(void)
{
    int transfers = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 96U));
    transfers = chip.transfers;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_BUSY, ee24_read(&ee, 0U, readback, 4U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_BUSY, ee24_write(&ee, 0U, pattern, 4U));
    TEST_ASSERT_EQUAL_INT(transfers, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mutex_takes, "a refused call took the mutex");

    TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE, ee24_wait(&ee),
                                  "a refused call took over the result of the job");
    TEST_ASSERT_EQUAL_MEMORY(pattern, chip.mem, 96U);
}

/*****************************************************************************************************/
/**
 * @brief ee24_wait() reports a call refused at once too, so simple code needs only it.
 */
void test_wait_reports_a_refused_call(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    /* Each refused call comes after one that worked, so its result cannot be
       left over from the call before. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 4U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size, pattern, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_wait(&ee));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 4U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, chip.size, readback, 1U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_wait(&ee));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 4U));
    lock_result = OSAL_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_read(&ee, 0U, readback, 4U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_wait(&ee));
}

/*****************************************************************************************************/
/**
 * @brief Each transfer gets the transfer time once for every 256 bytes it carries.
 *
 * Polling only, where the time reaches the HAL. With DMA it is ee24's own
 * limit, which test_a_stuck_dma_is_set_up_again() checks.
 */
void test_each_transfer_gets_time_for_its_size(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(EE24_TIMEOUT_TRANSFER_MS, chip.read_timeouts[0],
                                     "a few bytes did not get the whole of it");

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 32U));
    TEST_ASSERT_EQUAL_UINT32(EE24_TIMEOUT_TRANSFER_MS, chip.write_timeouts[0]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(EE24_TIMEOUT_TRANSFER_MS, chip.poll_timeout,
                                     "the one byte question got the wrong time");

    /* Only a build that reads more than 256 bytes in one call shows a piece
       getting more than one share. */
    if (READ_PIECE >= 32768U)
    {
        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 257U));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U * EE24_TIMEOUT_TRANSFER_MS, chip.read_timeouts[1],
                                         "a byte past 256 did not get a second share");

        /* Each half of a 24C512 is 32 KB, 128 times 256 bytes. */
        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));
        TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 65536U));
        TEST_ASSERT_EQUAL_UINT32(128U * EE24_TIMEOUT_TRANSFER_MS, chip.read_timeouts[0]);
        TEST_ASSERT_EQUAL_UINT32(128U * EE24_TIMEOUT_TRANSFER_MS, chip.read_timeouts[1]);
    }
}

/*****************************************************************************************************/
/**
 * @brief By polling a read goes a piece per call, none bigger than EE24_POLL_CHUNK.
 */
void test_a_polling_read_goes_a_piece_at_a_time(void)
{
    int pieces = (int)((1000U + READ_PIECE - 1U) / READ_PIECE);
    int i      = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    memcpy(&chip.mem[7], pattern, 1000U);

    /* The first piece is in before the call returns. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 7U, readback, 1000U));
    TEST_ASSERT_EQUAL_INT(1, chip.reads);
    TEST_ASSERT_EQUAL_INT_MESSAGE((pieces > 1) ? 1 : 0, mutex_held,
                                  "the mutex does not follow the read");

    /* The caller's state machine does other work between its questions. */
    while (ee24_is_running(&ee))
    {
        now_us += 100U;
    }

    TEST_ASSERT_EQUAL_INT(pieces, chip.reads);

    for (i = 0; i < chip.reads; i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(chip.read_sizes[i] <= READ_PIECE, "a piece was too big");
    }

    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 1000U);
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));

    /* ee24_wait() reads one piece after the other, with no sleep between. */
    memset(readback, 0, 1000U);
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(7U, readback, 1000U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 1000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sleeps, "slept between the pieces of a read");

    /* A read that fits in one piece is done when the call returns. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 16U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_held, "kept the mutex after a read in one piece");
    TEST_ASSERT_FALSE(ee24_is_running(&ee));
}

/*****************************************************************************************************/
/**
 * @brief A DMA read returns once it has started, and ee24_is_running() says when it is done.
 */
void test_a_dma_read_returns_at_once(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    memcpy(&chip.mem[7], pattern, 4096U);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 7U, readback, 4096U));
    TEST_ASSERT_EQUAL_INT(1, chip.dma_transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mutex_held, "gave the mutex back while the read was running");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(16U * EE24_TIMEOUT_TRANSFER_MS, ee.job_timeout,
                                     "4 KB did not get 16 shares of the transfer time");
    TEST_ASSERT_TRUE(ee24_is_running(&ee));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 4096U);
    TEST_ASSERT_FALSE(ee24_is_running(&ee));
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
}

/*****************************************************************************************************/
/**
 * @brief A DMA write sends each page only once the chip has stored the one before.
 */
void test_a_dma_write_goes_a_page_at_a_time(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 96U));
    TEST_ASSERT_EQUAL_INT(1, chip.dma_transfers);
    TEST_ASSERT_TRUE(ee24_is_running(&ee));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.polls, "asked the chip while the page was on its way");

    /* The first page is out, and the chip is storing it. */
    now_us += 2000U;
    TEST_ASSERT_TRUE(ee24_is_running(&ee));
    TEST_ASSERT_TRUE(ee24_is_running(&ee));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, chip.dma_transfers, "sent a page before the last was stored");

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));
    TEST_ASSERT_EQUAL_INT(3, chip.dma_transfers);
    TEST_ASSERT_EQUAL_MEMORY(pattern, chip.mem, 96U);
}

/*****************************************************************************************************/
/**
 * @brief A DMA transfer that never finishes ends at its time, and the I2C is set up again.
 */
void test_a_stuck_dma_is_set_up_again(void)
{
    uint32_t start = 0U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));

    chip.dma_stall = true;
    start          = HAL_GetTick();

    /* 1000 bytes are four shares of the transfer time. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, read_all(0U, readback, 1000U));
    TEST_ASSERT_EQUAL_INT(1, chip.i2c_resets);
    TEST_ASSERT_UINT32_WITHIN(2U, 4U * EE24_TIMEOUT_TRANSFER_MS, HAL_GetTick() - start);

    /* A page stuck on its way out gets one share. */
    start = HAL_GetTick();
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, write_all(0U, pattern, 16U));
    TEST_ASSERT_EQUAL_INT(2, chip.i2c_resets);
    TEST_ASSERT_UINT32_WITHIN(2U, EE24_TIMEOUT_TRANSFER_MS, HAL_GetTick() - start);
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a stuck page");
    TEST_ASSERT_EQUAL_INT(0, mutex_held);

    /* Set up again, the I2C works. */
    chip.dma_stall = false;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 16U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief A DMA transfer that ends in an error is reported, and ends the job.
 */
void test_a_dma_error_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));

    chip.dma_error = HAL_I2C_ERROR_DMA;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(0U, readback, 64U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 64U));
    TEST_ASSERT_EQUAL_INT(1, chip.writes);
    TEST_ASSERT_TRUE(chip.wp_high);
    TEST_ASSERT_EQUAL_INT(0, mutex_held);

    /* A piece after the first that the HAL will not start ends the read. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(16U, false));
    chip.dma_refuse_at = 2;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(200U, readback, 600U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, chip.reads, "kept reading after a piece would not start");
    TEST_ASSERT_EQUAL_INT(0, mutex_held);
    chip.dma_refuse_at = 0;

    /* A chip that stops answering shows up the same way, at the end. */
    chip.dma_error = HAL_I2C_ERROR_NONE;
    chip.present   = false;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, read_all(0U, readback, 64U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, write_all(0U, pattern, 64U));
}

/*****************************************************************************************************/
/**
 * @brief With every time HAL_MAX_DELAY, nothing ever runs out, and the HAL is told so.
 *
 * Multiplied for a long read, HAL_MAX_DELAY would wrap round to a short time.
 * A chip that takes two seconds a page, on a bus at 2 ms a byte, still gets
 * there.
 */
void test_for_ever_never_runs_out(void)
{
    int i = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));
    chip.write_cycle_us = 2000000U;
    chip.byte_us        = 2000U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, write_all(0U, pattern, 64U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, read_all(0U, readback, 65536U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 64U);

    for (i = 0; i < chip.writes; i++)
    {
        TEST_ASSERT_EQUAL_HEX32(HAL_MAX_DELAY, chip.write_timeouts[i]);
    }

    for (i = 0; i < chip.reads; i++)
    {
        TEST_ASSERT_EQUAL_HEX32(HAL_MAX_DELAY, chip.read_timeouts[i]);
    }

    TEST_ASSERT_EQUAL_HEX32(HAL_MAX_DELAY, chip.poll_timeout);

    /* Still waiting when the tick has gone all the way round, 49 days on,
       and not a millisecond short of for ever. No time passes on the bus, so
       the tick lands exactly one short of where the page's time started. */
    chip.byte_us = 0U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 1U));
    tick_jump_ms = 0xFFFFFFFFU;
    TEST_ASSERT_TRUE_MESSAGE(ee24_is_running(&ee), "for ever ran out after 49 days");
    tick_jump_ms = 0U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_wait(&ee));
}

/*****************************************************************************************************/
/**
 * @brief Run every test that fits this build.
 *
 * @return 0 when every test passed.
 */
int main(void)
{
    UNITY_BEGIN();

#if FOREVER
    /* Every other test expects the times ee24_config.h ships, and a chip that
       never finishes would never let go. */
    RUN_TEST(test_every_size_is_accepted);
    RUN_TEST(test_for_ever_never_runs_out);
#else
    RUN_TEST(test_every_size_is_accepted);
    RUN_TEST(test_a_size_that_does_not_exist_is_refused);
    RUN_TEST(test_null_pointers_are_caught_by_assert);
    RUN_TEST(test_a_chip_that_does_not_answer_is_reported);
    RUN_TEST(test_a_failed_init_leaves_the_handle_refused);
    RUN_TEST(test_the_wrong_address_is_reported);
    RUN_TEST(test_init_can_be_retried);
    RUN_TEST(test_a_handle_full_of_rubbish_works_after_init);
    RUN_TEST(test_the_last_byte_is_in_range);
    RUN_TEST(test_past_the_end_is_refused);
    RUN_TEST(test_a_range_that_wraps_is_refused);
    RUN_TEST(test_zero_bytes_does_nothing);
    RUN_TEST(test_every_size_reads_back_what_was_written);
    RUN_TEST(test_a_whole_24c512_reads_in_one_call);
    RUN_TEST(test_a_write_is_split_at_every_page);
    RUN_TEST(test_a_read_is_split_at_every_block);
    RUN_TEST(test_each_block_has_its_own_address);
    RUN_TEST(test_every_address_reaches_its_chip);
    RUN_TEST(test_the_pins_a_small_chip_ignores_are_ignored);
    RUN_TEST(test_a_write_waits_only_as_long_as_the_chip_needs);
    RUN_TEST(test_the_chip_is_asked_every_millisecond);
    RUN_TEST(test_the_chip_is_ready_when_the_wait_returns);
    RUN_TEST(test_a_chip_that_never_finishes_times_out);
    RUN_TEST(test_the_page_time_is_for_each_page);
    RUN_TEST(test_a_bus_too_slow_times_out);
    RUN_TEST(test_a_slow_bus_reads_a_whole_chip);
    RUN_TEST(test_a_failed_write_is_reported);
    RUN_TEST(test_a_failed_read_is_reported);
    RUN_TEST(test_write_protect_is_lifted_only_while_writing);
    RUN_TEST(test_write_protect_comes_back_after_an_error);
    RUN_TEST(test_no_pin_is_touched_without_write_protect);
    RUN_TEST(test_a_read_leaves_write_protect_alone);

    RUN_TEST(test_init_makes_one_mutex_for_a_chip_that_answers);
    RUN_TEST(test_a_mutex_that_cannot_be_made_is_reported);
    RUN_TEST(test_every_call_gives_the_mutex_back);
    RUN_TEST(test_a_bad_argument_takes_no_mutex);
    RUN_TEST(test_a_mutex_held_elsewhere_times_out);
    RUN_TEST(test_a_refused_mutex_is_reported);
    RUN_TEST(test_the_mutex_wait_is_the_configured_one);
    RUN_TEST(test_the_wait_sleeps_through_osal);

    RUN_TEST(test_nothing_started_is_not_running);
    RUN_TEST(test_a_write_returns_after_its_first_page);
    RUN_TEST(test_a_call_while_a_job_runs_is_refused);
    RUN_TEST(test_wait_reports_a_refused_call);

#if USES_DMA
    RUN_TEST(test_a_dma_read_returns_at_once);
    RUN_TEST(test_a_dma_write_goes_a_page_at_a_time);
    RUN_TEST(test_a_stuck_dma_is_set_up_again);
    RUN_TEST(test_a_dma_error_is_reported);
#else
    RUN_TEST(test_each_transfer_gets_time_for_its_size);
    RUN_TEST(test_a_polling_read_goes_a_piece_at_a_time);
#endif
#endif

    return UNITY_END();
}

/*
 * ****************************************************************************************************
 * Private function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Put a new chip of the given size on the bus, at 400 kHz.
 *
 * The page sizes are the smallest any maker uses for that size, so the model
 * catches a write that would wrap on any real part.
 *
 * @param[in] size_kbit  Size from the part name.
 */
static void chip_reset(uint16_t size_kbit)
{
    memset(&chip, 0, sizeof(chip));
    memset(chip.mem, 0xFF, sizeof(chip.mem));

    chip.size           = (uint32_t)size_kbit * 128U;
    chip.base_address   = EE24_ADDRESS_DEFAULT;
    chip.present        = true;
    chip.write_cycle_us = WRITE_CYCLE_US;
    chip.byte_us        = BYTE_US;
    chip.fault          = "none";

    if (size_kbit <= 2U)
    {
        chip.page = 8U;
    }
    else if (size_kbit <= 16U)
    {
        chip.page = 16U;
    }
    else if (size_kbit <= 64U)
    {
        chip.page = 32U;
    }
    else if (size_kbit <= 256U)
    {
        chip.page = 64U;
    }
    else
    {
        chip.page = 128U;
    }

    chip.mem_size = (size_kbit >= 32U) ? (uint16_t)I2C_MEMADD_SIZE_16BIT
                                       : (uint16_t)I2C_MEMADD_SIZE_8BIT;
}

/*****************************************************************************************************/
/**
 * @brief Make one call with a NULL pointer, and say whether assert_param stopped it.
 *
 * @param[in] which  0 to 7: init with no handle, init with no bus, read with
 *                   no handle, read with no buffer, the same for write, then
 *                   ee24_is_running() and ee24_wait() with no handle.
 * @return true when the call stopped at an assert instead of returning.
 */
static bool assert_stops(int which)
{
    bool stopped = true;

    assert_expected = true;

    if (setjmp(assert_return) == 0)
    {
        switch (which)
        {
            case 0:
                (void)ee24_init(NULL, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U);
                break;

            case 1:
                (void)ee24_init(&ee, NULL, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U);
                break;

            case 2:
                (void)ee24_read(NULL, 0U, readback, 1U);
                break;

            case 3:
                (void)ee24_read(&ee, 0U, NULL, 1U);
                break;

            case 4:
                (void)ee24_write(NULL, 0U, pattern, 1U);
                break;

            case 5:
                (void)ee24_write(&ee, 0U, NULL, 1U);
                break;

            case 6:
                (void)ee24_is_running(NULL);
                break;

            default:
                (void)ee24_wait(NULL);
                break;
        }

        /* Back here means no assert stopped it. */
        stopped = false;
    }

    assert_expected = false;

    return stopped;
}

/*****************************************************************************************************/
/**
 * @brief Forget what has happened so far, so a test sees only its own traffic.
 */
static void counts_clear(void)
{
    chip.transfers      = 0;
    chip.writes         = 0;
    chip.reads          = 0;
    chip.polls          = 0;
    chip.dma_transfers  = 0;
    chip.i2c_resets     = 0;
    chip.writes_ignored = 0;
    gpio_writes         = 0;
    mutex_takes         = 0;
    mutex_gives         = 0;
    mutex_held          = 0;
    mutex_last_wait     = 0U;
    sleeps              = 0;
    last_sleep_ms       = 0U;
}

/*****************************************************************************************************/
/**
 * @brief Note something a real chip, or the HAL, would have got wrong.
 *
 * @param[in] what  Says what it was, for the failure message.
 */
static void chip_fault(const char *what)
{
    if (chip.faults == 0)
    {
        chip.fault = what;
    }

    chip.faults++;
}

/*****************************************************************************************************/
/**
 * @brief Start a transfer: count it, and note one the HAL or a real driver would not allow.
 *
 * Every transfer after the mutex exists has to happen while it is held.
 * Init's own question comes before the mutex is made. Nothing may use the bus
 * while a DMA transfer is still on it.
 *
 * @param[in] hi2c  The handle the transfer was sent on.
 */
static void transfer_begins(const I2C_HandleTypeDef *hi2c)
{
    chip.transfers++;
    chip.i2c_error = HAL_I2C_ERROR_NONE;

    if (hi2c != &test_i2c)
    {
        chip_fault("a transfer went to the wrong I2C handle");
    }

    if ((mutex_created_at != NULL) && (mutex_held == 0))
    {
        chip_fault("a transfer ran without the mutex");
    }

    if (chip.dma_running)
    {
        chip_fault("the bus was used while a DMA transfer was still on it");
    }
}

/*****************************************************************************************************/
/**
 * @brief Spend a polling transfer's time on the bus, or as much as its timeout allows.
 *
 * @param[in] bytes    Bytes on the bus, address included.
 * @param[in] timeout  The HAL timeout, in milliseconds.
 * @return true when the transfer fitted in its timeout.
 */
static bool bus_time(uint32_t bytes, uint32_t timeout)
{
    uint64_t needed  = (uint64_t)bytes * chip.byte_us;
    uint64_t allowed = (uint64_t)timeout * 1000U;
    bool     fits    = (timeout == HAL_MAX_DELAY) || (needed <= allowed);

    if (fits)
    {
        now_us += (uint32_t)needed;
    }
    else
    {
        /* The HAL gives up at its timeout, part way, and says why. */
        now_us        += (uint32_t)allowed;
        chip.i2c_error = HAL_I2C_ERROR_TIMEOUT;
    }

    return fits;
}

/*****************************************************************************************************/
/**
 * @brief Start a DMA transfer, which ends once the clock has passed its bus time.
 *
 * @param[in] bytes  Bytes on the bus, address included.
 */
static void dma_begins(uint32_t bytes)
{
    chip.dma_transfers++;
    chip.dma_running = true;
    chip.dma_done_us = now_us + (bytes * chip.byte_us);
    chip.dma_result  = chip.dma_error;
}

/*****************************************************************************************************/
/**
 * @brief Note a write transfer's details.
 *
 * @param[in] dev_address  The I2C address it went to.
 * @param[in] size         Its length.
 * @param[in] timeout      The HAL timeout it was given, 0 by DMA.
 */
static void write_begins(uint16_t dev_address, uint16_t size, uint32_t timeout)
{
    if (chip.writes < (int)RECORD_MAX)
    {
        chip.write_sizes[chip.writes]    = size;
        chip.write_timeouts[chip.writes] = timeout;
    }

    chip.writes++;
    chip.last_dev_address = dev_address;
}

/*****************************************************************************************************/
/**
 * @brief Note a read transfer's details.
 *
 * @param[in] dev_address  The I2C address it went to.
 * @param[in] size         Its length.
 * @param[in] timeout      The HAL timeout it was given, 0 by DMA.
 */
static void read_begins(uint16_t dev_address, uint16_t size, uint32_t timeout)
{
    if (chip.reads < (int)RECORD_MAX)
    {
        chip.read_sizes[chip.reads]    = size;
        chip.read_timeouts[chip.reads] = timeout;
    }

    chip.reads++;
    chip.last_dev_address = dev_address;
}

/*****************************************************************************************************/
/**
 * @brief Whether the chip answers at all: there, and not busy storing a page.
 *
 * @return true when it would acknowledge.
 */
static bool chip_answers(void)
{
    return chip.present && (now_us >= chip.busy_until_us);
}

/*****************************************************************************************************/
/**
 * @brief Store a write in the modelled chip, the way a real one does.
 *
 * @param[in] dev_address  The 8 bit I2C address sent.
 * @param[in] mem_address  The memory address sent.
 * @param[in] mem_size     How many address bytes were sent, in HAL form.
 * @param[in] data         The bytes.
 * @param[in] size         How many.
 * @param[in] end_us       When the transfer ends, and the write cycle starts.
 * @return true when the chip acknowledged.
 */
static bool chip_store(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                       const uint8_t *data, uint16_t size, uint32_t end_us)
{
    bool     ok      = false;
    uint32_t address = 0U;

    if (chip.writes == chip.fail_write_at)
    {
        /* The test makes this one fail. */
    }
    else if (chip_answers() && chip_decode(dev_address, mem_address, mem_size, data, size, &address))
    {
        uint32_t offset = address % chip.page;
        uint32_t first  = address - offset;
        uint32_t i      = 0U;

        if ((offset + size) > chip.page)
        {
            chip_fault("a write ran past the end of its page and wrapped");
        }

        if (chip.wp_high)
        {
            chip.writes_ignored++;
        }
        else
        {
            for (i = 0U; i < size; i++)
            {
                chip.mem[first + ((offset + i) % chip.page)] = data[i];
            }

            chip.busy_until_us = end_us + chip.write_cycle_us;
        }

        ok = true;
    }
    else
    {
        /* No answer. */
    }

    return ok;
}

/*****************************************************************************************************/
/**
 * @brief Read from the modelled chip.
 *
 * A real chip keeps reading across blocks and wraps at the end of its memory.
 * The model does the same, but notes a read that crosses from one block to the
 * next, since the I2C address it went to only names the first block.
 *
 * @param[in]  dev_address  The 8 bit I2C address sent.
 * @param[in]  mem_address  The memory address sent.
 * @param[in]  mem_size     How many address bytes were sent, in HAL form.
 * @param[out] data         Where the bytes go.
 * @param[in]  size         How many.
 * @return true when the chip answered.
 */
static bool chip_load(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                      uint8_t *data, uint16_t size)
{
    bool     ok      = false;
    uint32_t address = 0U;

    if (chip.reads == chip.fail_read_at)
    {
        /* The test makes this one fail. */
    }
    else if (chip_answers() && chip_decode(dev_address, mem_address, mem_size, data, size, &address))
    {
        uint32_t i = 0U;

        if ((chip.mem_size == I2C_MEMADD_SIZE_8BIT) && (chip.size > 256U) &&
            (((address % 256U) + size) > 256U))
        {
            chip_fault("a read crossed into the next block");
        }

        for (i = 0U; i < size; i++)
        {
            data[i] = chip.mem[(address + i) % chip.size];
        }

        ok = true;
    }
    else
    {
        /* No answer. */
    }

    return ok;
}

/*****************************************************************************************************/
/**
 * @brief The bits of the I2C address that carry the block number, and not a pin.
 *
 * A chip from 24C04 to 24C16 answers on one address per block, the block
 * number sitting where A0 to A2 would, starting from A0.
 *
 * @return The block bits of the 8 bit address, 0 for a chip with one block.
 */
static uint16_t chip_block_bits(void)
{
    uint16_t block_bits = 0U;

    if ((chip.mem_size == I2C_MEMADD_SIZE_8BIT) && (chip.size > 256U))
    {
        block_bits = (uint16_t)(((chip.size / 256U) - 1U) << 1U);
    }

    return block_bits;
}

/*****************************************************************************************************/
/**
 * @brief Whether an I2C address is one of the chip's own.
 *
 * @param[in] dev_address  The 8 bit address sent.
 * @return true when it is one of this chip's.
 */
static bool chip_addressed(uint16_t dev_address)
{
    return (dev_address & (uint16_t)~chip_block_bits()) == chip.base_address;
}

/*****************************************************************************************************/
/**
 * @brief Turn what the HAL was given into an address in the chip.
 *
 * @param[in]  dev_address  The 8 bit I2C address sent.
 * @param[in]  mem_address  The memory address sent.
 * @param[in]  mem_size     How many address bytes were sent, in HAL form.
 * @param[in]  data         The HAL's data pointer.
 * @param[in]  size         The HAL's length.
 * @param[out] address      Where in the chip that is.
 * @return true when the chip answers and understood the request.
 */
static bool chip_decode(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                        const uint8_t *data, uint16_t size, uint32_t *address)
{
    bool ok = false;

    if (!chip_addressed(dev_address))
    {
        /* Nobody at that address, so nobody answers. */
    }
    else if ((data == NULL) || (size == 0U))
    {
        chip_fault("the HAL was handed no data, which it refuses");
    }
    else if (mem_size != chip.mem_size)
    {
        chip_fault("sent the wrong number of address bytes for this size");
    }
    else if ((chip.mem_size == I2C_MEMADD_SIZE_8BIT) && (mem_address > 0xFFU))
    {
        chip_fault("a one byte address did not fit in one byte");
    }
    else
    {
        uint32_t block = 0U;

        /* Only the block bits, since a pin tied high is not part of the
           block number. */
        block = ((uint32_t)dev_address & chip_block_bits()) >> 1U;

        *address = (block * 256U) + mem_address;

        if (*address >= chip.size)
        {
            chip_fault("an address past the end of the chip");
        }
        else
        {
            ok = true;
        }
    }

    return ok;
}

/*****************************************************************************************************/
/**
 * @brief Put a new chip on the bus and hand it to ee24_init().
 *
 * The counts start from zero afterwards, so a test sees only its own traffic.
 *
 * @param[in] size_kbit  Size from the part name.
 * @param[in] with_wp    Whether the write protect pin is wired.
 * @return What ee24_init() returned.
 */
static ee24_err_t init_chip(uint16_t size_kbit, bool with_wp)
{
    ee24_err_t err = EE24_ERR_NONE;

    chip_reset(size_kbit);

    /* A new handle, so the mutex a previous init made is not this one's. */
    mutex_created_at = NULL;

    err = ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, size_kbit,
                    with_wp ? &test_wp_port : NULL, with_wp ? WP_PIN : 0U);

    counts_clear();

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Put a new chip on the bus at an address of its own, and hand ee24_init() another.
 *
 * @param[in] size_kbit     Size from the part name.
 * @param[in] chip_address  Where the chip answers, its ignored pins as 0.
 * @param[in] dev_address   What ee24_init() is given.
 * @return What ee24_init() returned.
 */
static ee24_err_t init_chip_at(uint16_t size_kbit, uint16_t chip_address, uint8_t dev_address)
{
    ee24_err_t err = EE24_ERR_NONE;

    chip_reset(size_kbit);
    chip.base_address = chip_address;

    /* A new handle, so the mutex a previous init made is not this one's. */
    mutex_created_at = NULL;

    err = ee24_init(&ee, &test_i2c, dev_address, size_kbit, NULL, 0U);

    counts_clear();

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Finish a read or write the way a user would: wait until it has ended.
 *
 * @param[in] err  What the call returned.
 * @return How it ended: the call's own error when it started nothing.
 */
static ee24_err_t settle(ee24_err_t err)
{
    if (err == EE24_ERR_NONE)
    {
        err = ee24_wait(&ee);
    }

    return err;
}

/*****************************************************************************************************/
/**
 * @brief Read, and wait until the read has finished.
 *
 * @return How it ended.
 */
static ee24_err_t read_all(uint32_t address, uint8_t *data, size_t len)
{
    return settle(ee24_read(&ee, address, data, len));
}

/*****************************************************************************************************/
/**
 * @brief Write, and wait until the last page is stored.
 *
 * @return How it ended.
 */
static ee24_err_t write_all(uint32_t address, const uint8_t *data, size_t len)
{
    return settle(ee24_write(&ee, address, data, len));
}

/*****************************************************************************************************/
/**
 * @brief Fill pattern with bytes that do not repeat on any page or block boundary.
 *
 * @param[in] seed  Changes the bytes from one call to the next.
 */
static void pattern_fill(uint32_t seed)
{
    uint32_t state = seed + 1U;
    uint32_t i     = 0U;

    for (i = 0U; i < CHIP_MAX_BYTES; i++)
    {
        state      = (state * 1103515245U) + 12345U;
        pattern[i] = (uint8_t)(state >> 16U);
    }
}
