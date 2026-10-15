# Unity is vendored in test/unity. It is third party code, so it is built as its
# own target and is deliberately left out of this project's -Werror.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

set(EE24_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wundef)

# ee24 reaches the RTOS only through osal, and the fake osal in fake/ lets the
# tests hold or refuse the mutex at will, so there is no build per RTOS: those
# are osal's tests to run. The fake stands for an RTOS unless a build says
# OSAL_RTOS_NONE, where nothing may sleep and the fake HAL moves the clock on a
# microsecond for each question, as a real CPU spends it.

# One build of the suite, with the settings given. Defining EE24_CONFIG_H, the
# include guard of ee24_config.h, skips the shipped file, and the build then
# gives every setting itself. The definitions are PUBLIC so the test file is
# built with the same settings as the driver it tests.
function(ee24_suite name)
    add_library(ee24_${name} STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
    target_include_directories(ee24_${name} PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${EE24_CONFIG_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/fake)
    target_compile_definitions(ee24_${name} PUBLIC ${ARGN})
    target_compile_features(ee24_${name} PUBLIC c_std_11)

    add_executable(test_ee24_${name} test_ee24.c)
    target_link_libraries(test_ee24_${name} PRIVATE ee24_${name} unity)

    # The stubs stand in for the CubeMX main.h and the HAL.
    target_include_directories(test_ee24_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ee24_${name} PRIVATE ${EE24_WARNINGS})
        target_compile_options(test_ee24_${name} PRIVATE ${EE24_WARNINGS})
    endif()

    add_test(NAME ee24_${name} COMMAND test_ee24_${name})
endfunction()

# The times ee24_config.h ships.
set(EE24_TIMES
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=35U
    EE24_TIMEOUT_PAGE_MS=15U)

# With ee24_config.h exactly as it ships: the calls that wait, by polling.
add_executable(test_ee24 test_ee24.c)
target_link_libraries(test_ee24 PRIVATE ee24 unity)
target_include_directories(test_ee24 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ee24 PRIVATE ${EE24_WARNINGS})
    target_compile_options(test_ee24 PRIVATE ${EE24_WARNINGS})
endif()

add_test(NAME ee24_blocking COMMAND test_ee24)

# The calls that wait, by DMA.
ee24_suite(dma
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_DMA
    ${EE24_TIMES})

# Jobs, by polling and by DMA.
ee24_suite(nb
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32U
    ${EE24_TIMES})

ee24_suite(nb_dma
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_DMA
    ${EE24_TIMES})

# Jobs by polling, reading 32 KB in one call, the most EE24_POLL_CHUNK allows.
# With the shipped 32 bytes every piece gets one share of the transfer time, so
# this is the build where a piece of a job gets time for its size by polling.
ee24_suite(nb_big_chunk
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32768U
    ${EE24_TIMES})

# HAL_MAX_DELAY for the transfer and the page times, which waits for ever as it
# does in the HAL. Only the tests about that run in this build: every other
# one expects the times ee24_config.h ships, and a chip that never finishes
# would never let it go. Jobs by polling, where the time reaches the HAL and a
# test can move the tick on in the middle of a job.
ee24_suite(nb_forever
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32768U
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_PAGE_MS=HAL_MAX_DELAY)

# No RTOS, for each mode and each transfer.
ee24_suite(bare
    OSAL_RTOS=OSAL_RTOS_NONE)

ee24_suite(bare_dma
    OSAL_RTOS=OSAL_RTOS_NONE
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_DMA
    ${EE24_TIMES})

ee24_suite(bare_nb
    OSAL_RTOS=OSAL_RTOS_NONE
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32U
    ${EE24_TIMES})

ee24_suite(bare_nb_dma
    OSAL_RTOS=OSAL_RTOS_NONE
    EE24_CONFIG_H
    EE24_MODE=EE24_MODE_NON_BLOCKING
    EE24_TRANSFER=EE24_TRANSFER_DMA
    ${EE24_TIMES})
