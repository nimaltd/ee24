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
 *              checked instantly. osal is a fake the tests control, so a
 *              mutex held by another thread or refused by the RTOS is
 *              tested here too, with no RTOS at all.
 */

/*
 * ****************************************************************************************************
 * Includes
 * ****************************************************************************************************
*/

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

/* Big enough for the largest chip, a 24C512. */
#define CHIP_MAX_BYTES      65536U

/* How long the modelled chip takes to store a page. Datasheets give 5 ms as the
   most, and a real part is usually quicker. */
#define WRITE_CYCLE_US      3500U

/* One byte on the bus at 400 kHz, near enough. */
#define BYTE_US             25U

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
 * @brief A 24xx chip on the bus, and what happened to it.
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
    int         fail_write_at;                /**< Write transfer that fails, 0 for none. */
    int         fail_read_at;                 /**< Read transfer that fails, 0 for none. */
    int         transfers;                    /**< Every transfer of any kind.           */
    int         writes;                       /**< Write transfers.                      */
    int         reads;                        /**< Read transfers.                       */
    int         polls;                        /**< Is it ready? questions.               */
    int         writes_ignored;               /**< Writes sent while WP was high.        */
    int         faults;                       /**< Things a real chip would get wrong.   */
    const char  *fault;                       /**< What the first of them was.           */
    uint16_t    last_dev_address;             /**< I2C address of the last transfer.     */
    uint16_t    write_sizes[RECORD_MAX];      /**< Length of each write transfer.        */
    uint32_t    write_timeouts[RECORD_MAX];   /**< Timeout given with each one.          */
    uint16_t    read_sizes[RECORD_MAX];       /**< Length of each read transfer.         */
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

static uint32_t          now_us      = 0U;
static int               gpio_writes = 0;

static uint8_t           pattern[CHIP_MAX_BYTES];
static uint8_t           readback[CHIP_MAX_BYTES];

/* The fake osal. */
static osal_err_t         lock_result        = OSAL_ERR_NONE;
static uint32_t           lock_wait_ms       = 0U;
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
static void       counts_clear(void);
static void       chip_fault(const char *what);
static bool       transfer_begins(const I2C_HandleTypeDef *hi2c, uint32_t bytes);
static bool       chip_addressed(uint16_t dev_address);
static bool       chip_decode(uint16_t dev_address, uint16_t mem_address, uint16_t mem_size,
                              const uint8_t *data, uint16_t size, uint32_t *address);
static ee24_err_t init_chip(uint16_t size_kbit, bool with_wp);
static void       pattern_fill(uint32_t seed);

/*
 * ****************************************************************************************************
 * Public function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Return the tick, from the tests' clock.
 *
 * @return Milliseconds since the clock was reset.
 */
uint32_t HAL_GetTick(void)
{
    return now_us / 1000U;
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
 * @brief Write to the modelled chip.
 *
 * The data wraps inside its page, the way a real chip stores it. A write sent
 * while WP is high is acknowledged and then ignored, which is also what a real
 * chip does, so a write protect pin left high shows up as data that never
 * arrived.
 *
 * @return HAL_OK when the chip took the data, HAL_ERROR when it did not answer.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                    uint16_t Size, uint32_t Timeout)
{
    HAL_StatusTypeDef status  = HAL_ERROR;
    uint32_t          address = 0U;

    if (chip.writes < (int)RECORD_MAX)
    {
        chip.write_sizes[chip.writes]    = Size;
        chip.write_timeouts[chip.writes] = Timeout;
    }

    chip.writes++;
    chip.last_dev_address = DevAddress;

    if (chip.writes == chip.fail_write_at)
    {
        chip.transfers++;
    }
    else if (transfer_begins(hi2c, 3U + Size) &&
             chip_decode(DevAddress, MemAddress, MemAddSize, pData, Size, &address))
    {
        uint32_t offset = address % chip.page;
        uint32_t first  = address - offset;
        uint32_t i      = 0U;

        if ((offset + Size) > chip.page)
        {
            chip_fault("a write ran past the end of its page and wrapped");
        }

        if (chip.wp_high)
        {
            chip.writes_ignored++;
        }
        else
        {
            for (i = 0U; i < Size; i++)
            {
                chip.mem[first + ((offset + i) % chip.page)] = pData[i];
            }

            chip.busy_until_us = now_us + chip.write_cycle_us;
        }

        status = HAL_OK;
    }
    else
    {
        /* No answer. */
    }

    return status;
}

/*****************************************************************************************************/
/**
 * @brief Read from the modelled chip.
 *
 * A real chip keeps reading across blocks and wraps at the end of its memory.
 * The model does the same, but notes a read that crosses from one block to the
 * next, since the I2C address it went to only names the first block.
 *
 * @return HAL_OK when the chip answered, HAL_ERROR when it did not.
 */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout)
{
    HAL_StatusTypeDef status  = HAL_ERROR;
    uint32_t          address = 0U;

    (void)Timeout;

    if (chip.reads < (int)RECORD_MAX)
    {
        chip.read_sizes[chip.reads] = Size;
    }

    chip.reads++;
    chip.last_dev_address = DevAddress;

    if (chip.reads == chip.fail_read_at)
    {
        chip.transfers++;
    }
    else if (transfer_begins(hi2c, 4U + Size) &&
             chip_decode(DevAddress, MemAddress, MemAddSize, pData, Size, &address))
    {
        uint32_t i = 0U;

        if ((chip.mem_size == I2C_MEMADD_SIZE_8BIT) && (chip.size > 256U) &&
            (((address % 256U) + Size) > 256U))
        {
            chip_fault("a read crossed into the next block");
        }

        for (i = 0U; i < Size; i++)
        {
            pData[i] = chip.mem[(address + i) % chip.size];
        }

        status = HAL_OK;
    }
    else
    {
        /* No answer. */
    }

    return status;
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

    (void)Timeout;

    for (trial = 0U; (trial < Trials) && (status != HAL_OK); trial++)
    {
        if (chip.polls < (int)RECORD_MAX)
        {
            chip.poll_times_us[chip.polls] = now_us;
        }

        chip.polls++;

        if (transfer_begins(hi2c, 1U) && chip_addressed(DevAddress))
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

    /* Another thread had the chip for this long. */
    now_us += lock_wait_ms * 1000U;

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
    lock_result        = OSAL_ERR_NONE;
    lock_wait_ms       = 0U;
    mutex_create_fails = false;
    mutex_created_at   = NULL;
    mutex_creates      = 0;

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U, 100U),
                                  "a handle with a bad size was used");
}

/*****************************************************************************************************/
/**
 * @brief NULL pointers are refused, never followed.
 */
void test_null_pointers_are_refused(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID,
                          ee24_init(NULL, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID,
                          ee24_init(&ee, NULL, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(NULL, 0U, readback, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, NULL, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_write(NULL, 0U, pattern, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_write(&ee, 0U, NULL, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
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

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_write(&ee, 0U, pattern, 1U, 100U));
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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U, 100U));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.present = false;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C,
                          ee24_init(&ee, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U, 100U));
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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 4U, 100U));
}

/*****************************************************************************************************/
/**
 * @brief A handle full of rubbish works once ee24_init() has run.
 *
 * Version 3 never cleared its lock flag in init, so a handle declared as a
 * local variable could start out locked and hang the first call for good.
 */
void test_a_handle_full_of_rubbish_works_after_init(void)
{
    ee24_t local;

    memset(&local, 0xA5, sizeof(local));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE,
                          ee24_init(&local, &test_i2c, EE24_ADDRESS_DEFAULT, 256U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&local, 10U, pattern, 8U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&local, 10U, readback, 8U, 100U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 8U);
}

/*****************************************************************************************************/
/**
 * @brief The last byte of the chip can be written and read.
 */
void test_the_last_byte_is_in_range(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, chip.size - 1U, pattern, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, chip.size - 1U, readback, 1U, 100U));
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

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size - 1U, pattern, 2U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size, pattern, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, chip.size - 1U, readback, 2U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 0U, readback, chip.size + 1U, 100U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.transfers, "the bus was used for a bad range");
}

/*****************************************************************************************************/
/**
 * @brief A range whose end wraps past zero is still refused.
 */
void test_a_range_that_wraps_is_refused(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 0xFFFFFFFFU, readback, 2U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_read(&ee, 16U, readback, SIZE_MAX, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, 16U, pattern, SIZE_MAX - 8U, 100U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
}

/*****************************************************************************************************/
/**
 * @brief Nothing to do is done at once, and succeeds.
 */
void test_zero_bytes_does_nothing(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 0U, 100U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
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

        TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE,
                                      ee24_write(&ee, 3U, pattern, len, HAL_MAX_DELAY),
                                      "the write failed");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, &chip.mem[3], len,
                                         "the chip holds something else");
        TEST_ASSERT_EQUAL_INT_MESSAGE(EE24_ERR_NONE,
                                      ee24_read(&ee, 3U, readback, len, HAL_MAX_DELAY),
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

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 65536U, HAL_MAX_DELAY));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief A write that starts inside a page is split where the pages end.
 */
void test_a_write_is_split_at_every_page(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(32U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 30U, pattern, 40U, 100U));

    TEST_ASSERT_EQUAL_INT(3, chip.writes);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2U, chip.write_sizes[0], "did not stop at the first page end");
    TEST_ASSERT_EQUAL_UINT16(32U, chip.write_sizes[1]);
    TEST_ASSERT_EQUAL_UINT16(6U, chip.write_sizes[2]);
}

/*****************************************************************************************************/
/**
 * @brief A read on a 24C16 stops at the end of each 256 byte block.
 */
void test_a_read_is_split_at_every_block(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(16U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 200U, readback, 600U, 100U));

    TEST_ASSERT_EQUAL_INT(4, chip.reads);
    TEST_ASSERT_EQUAL_UINT16(56U, chip.read_sizes[0]);
    TEST_ASSERT_EQUAL_UINT16(256U, chip.read_sizes[1]);
    TEST_ASSERT_EQUAL_UINT16(256U, chip.read_sizes[2]);
    TEST_ASSERT_EQUAL_UINT16(32U, chip.read_sizes[3]);
}

/*****************************************************************************************************/
/**
 * @brief Chips from 24C04 to 24C16 find each block at its own I2C address.
 */
void test_each_block_has_its_own_address(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(16U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0x7FFU, pattern, 1U, 100U));
    TEST_ASSERT_EQUAL_HEX16(0xAEU, chip.last_dev_address);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(4U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0x100U, readback, 1U, 100U));
    TEST_ASSERT_EQUAL_HEX16(0xA2U, chip.last_dev_address);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(2U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0xFFU, readback, 1U, 100U));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xA0U, chip.last_dev_address, "a 24C02 has only one block");
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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 320U, 1000U));

    TEST_ASSERT_EQUAL_INT(10, chip.writes);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(15U, 50U, HAL_GetTick() - start,
                                      "the write took far longer or shorter than the chip needed");
}

/*****************************************************************************************************/
/**
 * @brief The chip is asked every millisecond, and never sooner.
 */
void test_the_chip_is_asked_every_millisecond(void)
{
    int i = 0;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 96U, 1000U));

    /* Three pages, each stored in 3.5 ms, so each is asked about 4 times: at
       1, 2, 3 and 4 ms. Asking less often would wait longer than it needs. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, chip.polls, "not asked once a millisecond");
    TEST_ASSERT_EQUAL_INT_MESSAGE(chip.polls, sleeps, "asked without sleeping first");

    for (i = 1; (i < chip.polls) && (i < (int)RECORD_MAX); i++)
    {
        TEST_ASSERT_TRUE_MESSAGE((chip.poll_times_us[i] - chip.poll_times_us[i - 1]) >= 1000U,
                                 "asked again in under a millisecond");
    }
}

/*****************************************************************************************************/
/**
 * @brief The chip has finished storing when a write returns.
 */
void test_the_chip_is_ready_when_a_write_returns(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 5U, 100U));

    TEST_ASSERT_TRUE_MESSAGE(chip.busy_until_us <= now_us, "returned while the chip was busy");
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 5U, 100U));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, 5U);
}

/*****************************************************************************************************/
/**
 * @brief A chip that never finishes a write ends in a timeout, on time.
 */
void test_a_chip_that_never_finishes_times_out(void)
{
    uint32_t start = 0U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.write_cycle_us = 0xF0000000U;

    start = HAL_GetTick();
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 1U, 50U));
    TEST_ASSERT_UINT32_WITHIN(2U, 51U, HAL_GetTick() - start);
}

/*****************************************************************************************************/
/**
 * @brief The timeout holds for the whole call, not for each page.
 */
void test_the_timeout_covers_the_whole_call(void)
{
    uint32_t start = 0U;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.write_cycle_us = 9000U;

    start = HAL_GetTick();
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 320U, 30U));
    TEST_ASSERT_TRUE_MESSAGE((HAL_GetTick() - start) <= 33U, "ran well past the timeout");
    TEST_ASSERT_TRUE(chip.writes < 10);
}

/*****************************************************************************************************/
/**
 * @brief Each transfer is given only the time that is left.
 */
void test_each_transfer_gets_only_the_time_left(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 96U, 1000U));

    TEST_ASSERT_EQUAL_UINT32(1000U, chip.write_timeouts[0]);
    TEST_ASSERT_TRUE_MESSAGE(chip.write_timeouts[2] < chip.write_timeouts[1], "not counting down");
    TEST_ASSERT_TRUE(chip.write_timeouts[1] < 1000U);
}

/*****************************************************************************************************/
/**
 * @brief A failed write is reported, and nothing more is sent after it.
 */
void test_a_failed_write_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    chip.fail_write_at = 2;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_write(&ee, 0U, pattern, 320U, 1000U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, chip.writes, "kept writing after a failure");
}

/*****************************************************************************************************/
/**
 * @brief A failed read is reported, and nothing more is asked after it.
 */
void test_a_failed_read_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(512U, false));
    chip.fail_read_at = 1;

    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_read(&ee, 0U, readback, 65536U, 1000U));
    TEST_ASSERT_EQUAL_INT(1, chip.reads);
}

/*****************************************************************************************************/
/**
 * @brief Write protect is lifted only while a write runs, and the data lands.
 */
void test_write_protect_is_lifted_only_while_writing(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "init left the chip unprotected");

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 100U, pattern, 70U, 1000U));

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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_write(&ee, 0U, pattern, 96U, 1000U));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a failed write");

    chip.fail_write_at  = 0;
    chip.write_cycle_us = 0xF0000000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 1U, 20U));
    TEST_ASSERT_TRUE_MESSAGE(chip.wp_high, "unprotected after a timeout");
}

/*****************************************************************************************************/
/**
 * @brief Without a write protect pin, no pin is ever touched.
 */
void test_no_pin_is_touched_without_write_protect(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 40U, 1000U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 40U, 1000U));

    TEST_ASSERT_EQUAL_INT(0, gpio_writes);
}

/*****************************************************************************************************/
/**
 * @brief A read leaves write protect alone.
 */
void test_a_read_leaves_write_protect_alone(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, true));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 40U, 1000U));

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

    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, readback, 1U, 100U));
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

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 40U, 1000U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 40U, 1000U));

    chip.fail_write_at = chip.writes + 1;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_write(&ee, 0U, pattern, 40U, 1000U));

    chip.fail_read_at = chip.reads + 1;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_I2C, ee24_read(&ee, 0U, readback, 40U, 1000U));

    chip.fail_write_at  = 0;
    chip.write_cycle_us = 0xF0000000U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 1U, 20U));

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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_RANGE, ee24_write(&ee, chip.size, pattern, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_INVALID, ee24_read(&ee, 0U, NULL, 1U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 0U, 100U));

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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_write(&ee, 0U, pattern, 4U, 100U));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_read(&ee, 0U, readback, 4U, 0U));

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
    TEST_ASSERT_EQUAL_INT(EE24_ERR_MUTEX, ee24_read(&ee, 0U, readback, 4U, 100U));

    /* osal answers this only for a NULL mutex, which ee24 never passes, but
       it must not read as success either. */
    lock_result = OSAL_ERR_INVALID;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_MUTEX, ee24_write(&ee, 0U, pattern, 4U, 100U));

    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mutex_gives, "gave back a mutex it never had");
}

/*****************************************************************************************************/
/**
 * @brief The wait for the mutex counts against the timeout.
 */
void test_the_wait_for_the_mutex_counts_against_the_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    /* Another thread keeps the chip for 20 ms, the whole timeout. */
    lock_wait_ms = 20U;
    TEST_ASSERT_EQUAL_INT(EE24_ERR_TIMEOUT, ee24_read(&ee, 0U, readback, 4U, 20U));
    TEST_ASSERT_EQUAL_INT(0, chip.transfers);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mutex_gives, "kept the mutex after running out of time");

    /* One millisecond more, and there is time left to read. */
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 4U, 21U));
}

/*****************************************************************************************************/
/**
 * @brief While a write waits for the chip, it sleeps through osal, a millisecond at a time.
 */
void test_a_write_sleeps_through_osal(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_write(&ee, 0U, pattern, 40U, 1000U));

    TEST_ASSERT_TRUE(sleeps > 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, last_sleep_ms, "a poll should sleep one millisecond");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, chip.faults, chip.fault);
}

/*****************************************************************************************************/
/**
 * @brief The wait for the mutex follows the timeout, and HAL_MAX_DELAY waits forever.
 */
void test_the_mutex_wait_follows_the_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, init_chip(256U, false));

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 4U, 25U));
    TEST_ASSERT_EQUAL_UINT32(25U, mutex_last_wait);

    TEST_ASSERT_EQUAL_INT(EE24_ERR_NONE, ee24_read(&ee, 0U, readback, 4U, HAL_MAX_DELAY));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xFFFFFFFFU, mutex_last_wait, "should wait forever");
}

/*****************************************************************************************************/
/**
 * @brief Run every test.
 *
 * @return 0 when every test passed.
 */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_every_size_is_accepted);
    RUN_TEST(test_a_size_that_does_not_exist_is_refused);
    RUN_TEST(test_null_pointers_are_refused);
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
    RUN_TEST(test_a_write_waits_only_as_long_as_the_chip_needs);
    RUN_TEST(test_the_chip_is_asked_every_millisecond);
    RUN_TEST(test_the_chip_is_ready_when_a_write_returns);
    RUN_TEST(test_a_chip_that_never_finishes_times_out);
    RUN_TEST(test_the_timeout_covers_the_whole_call);
    RUN_TEST(test_each_transfer_gets_only_the_time_left);
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
    RUN_TEST(test_the_wait_for_the_mutex_counts_against_the_timeout);
    RUN_TEST(test_a_write_sleeps_through_osal);
    RUN_TEST(test_the_mutex_wait_follows_the_timeout);

    return UNITY_END();
}

/*
 * ****************************************************************************************************
 * Private function implementations
 * ****************************************************************************************************
*/

/*****************************************************************************************************/
/**
 * @brief Put a new chip of the given size on the bus.
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
 * @brief Forget what has happened so far, so a test sees only its own traffic.
 */
static void counts_clear(void)
{
    chip.transfers      = 0;
    chip.writes         = 0;
    chip.reads          = 0;
    chip.polls          = 0;
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
 * @brief Note something a real chip would have got wrong.
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
 * @brief Start a transfer: count it, spend its bus time, and say whether the chip answers.
 *
 * Every transfer after the mutex exists has to happen while it is held.
 * Init's own question comes before the mutex is made.
 *
 * @param[in] hi2c   The handle the transfer was sent on.
 * @param[in] bytes  Bytes on the bus, address included.
 * @return true when the chip is there and not busy storing a page.
 */
static bool transfer_begins(const I2C_HandleTypeDef *hi2c, uint32_t bytes)
{
    chip.transfers++;
    now_us += bytes * BYTE_US;

    if (hi2c != &test_i2c)
    {
        chip_fault("a transfer went to the wrong I2C handle");
    }

    if ((mutex_created_at != NULL) && (mutex_held == 0))
    {
        chip_fault("a transfer ran without the mutex");
    }

    return chip.present && (now_us >= chip.busy_until_us);
}

/*****************************************************************************************************/
/**
 * @brief Whether an I2C address is one of the chip's own.
 *
 * A chip from 24C04 to 24C16 answers on one address per block, the block
 * number sitting where A0 to A2 would.
 *
 * @param[in] dev_address  The 8 bit address sent.
 * @return true when it is one of this chip's.
 */
static bool chip_addressed(uint16_t dev_address)
{
    uint16_t block_bits = 0U;

    if ((chip.mem_size == I2C_MEMADD_SIZE_8BIT) && (chip.size > 256U))
    {
        block_bits = (uint16_t)(((chip.size / 256U) - 1U) << 1U);
    }

    return (dev_address & (uint16_t)~block_bits) == chip.base_address;
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

        if (chip.mem_size == I2C_MEMADD_SIZE_8BIT)
        {
            block = ((uint32_t)dev_address >> 1U) & 0x07U;
        }

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
