# Unity is vendored in test/unity. It is third party code, so it is built as its
# own target and is deliberately left out of this project's -Werror.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

set(EE24_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wundef)

# ee24 reaches the RTOS only through osal, and the fake osal in fake/ lets the
# tests hold or refuse the mutex at will, so there is no build per RTOS: those
# are osal's tests to run. There is one per EE24_TRANSFER setting, one reading
# 32 KB a call by polling, one for times that never run out, and two with no
# RTOS, where the wait does not sleep.

# Polling, with ee24_config.h exactly as it ships.
add_executable(test_ee24 test_ee24.c)
target_link_libraries(test_ee24 PRIVATE ee24 unity)

# DMA. Defining the include guard of ee24_config.h skips the shipped file, and
# the settings are given here instead, the timeouts as it ships them. The
# definitions are PUBLIC so the test file is built with the same settings as
# the driver it tests.
add_library(ee24_dma STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
target_include_directories(ee24_dma PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${EE24_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake)
target_compile_definitions(ee24_dma PUBLIC
    EE24_CONFIG_H
    EE24_TRANSFER=EE24_TRANSFER_DMA
    EE24_POLL_CHUNK=32U
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=35U
    EE24_TIMEOUT_PAGE_MS=15U)
target_compile_features(ee24_dma PUBLIC c_std_11)

add_executable(test_ee24_dma test_ee24.c)
target_link_libraries(test_ee24_dma PRIVATE ee24_dma unity)

# Polling, reading 32 KB in one call, the most EE24_POLL_CHUNK allows. With the
# shipped 32 bytes every piece gets one share of the transfer time, so this is
# the build where a piece gets time for its size by polling.
add_library(ee24_big_chunk STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
target_include_directories(ee24_big_chunk PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${EE24_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake)
target_compile_definitions(ee24_big_chunk PUBLIC
    EE24_CONFIG_H
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32768U
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=35U
    EE24_TIMEOUT_PAGE_MS=15U)
target_compile_features(ee24_big_chunk PUBLIC c_std_11)

add_executable(test_ee24_big_chunk test_ee24.c)
target_link_libraries(test_ee24_big_chunk PRIVATE ee24_big_chunk unity)

# HAL_MAX_DELAY for the transfer and the page times, which waits for ever as it
# does in the HAL. Only the tests about that run in this build: every other
# one expects the times ee24_config.h ships, and a chip that never finishes
# would never let it go. By polling, where the time reaches the HAL.
add_library(ee24_forever STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
target_include_directories(ee24_forever PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${EE24_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake)
target_compile_definitions(ee24_forever PUBLIC
    EE24_CONFIG_H
    EE24_TRANSFER=EE24_TRANSFER_POLLING
    EE24_POLL_CHUNK=32768U
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_PAGE_MS=HAL_MAX_DELAY)
target_compile_features(ee24_forever PUBLIC c_std_11)

add_executable(test_ee24_forever test_ee24.c)
target_link_libraries(test_ee24_forever PRIVATE ee24_forever unity)

# No RTOS, by polling with ee24_config.h as it ships, and by DMA. ee24_wait()
# then takes the next step at once instead of sleeping, and the fake HAL moves
# the clock on a microsecond for each question, as a real CPU spends it.
add_library(ee24_bare STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
target_include_directories(ee24_bare PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${EE24_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake)
target_compile_definitions(ee24_bare PUBLIC OSAL_RTOS=OSAL_RTOS_NONE)
target_compile_features(ee24_bare PUBLIC c_std_11)

add_executable(test_ee24_bare test_ee24.c)
target_link_libraries(test_ee24_bare PRIVATE ee24_bare unity)

add_library(ee24_bare_dma STATIC ${PROJECT_SOURCE_DIR}/src/ee24.c)
target_include_directories(ee24_bare_dma PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${EE24_CONFIG_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake)
target_compile_definitions(ee24_bare_dma PUBLIC
    OSAL_RTOS=OSAL_RTOS_NONE
    EE24_CONFIG_H
    EE24_TRANSFER=EE24_TRANSFER_DMA
    EE24_POLL_CHUNK=32U
    EE24_TIMEOUT_MUTEX_MS=HAL_MAX_DELAY
    EE24_TIMEOUT_TRANSFER_MS=35U
    EE24_TIMEOUT_PAGE_MS=15U)
target_compile_features(ee24_bare_dma PUBLIC c_std_11)

add_executable(test_ee24_bare_dma test_ee24.c)
target_link_libraries(test_ee24_bare_dma PRIVATE ee24_bare_dma unity)

# The stubs stand in for the CubeMX main.h and the HAL.
foreach(target test_ee24 test_ee24_dma test_ee24_big_chunk test_ee24_forever test_ee24_bare
               test_ee24_bare_dma)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub)
endforeach()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target ee24 ee24_dma ee24_big_chunk ee24_forever ee24_bare ee24_bare_dma test_ee24
                   test_ee24_dma test_ee24_big_chunk test_ee24_forever test_ee24_bare
                   test_ee24_bare_dma)
        target_compile_options(${target} PRIVATE ${EE24_WARNINGS})
    endforeach()
endif()

add_test(NAME ee24_polling COMMAND test_ee24)
add_test(NAME ee24_dma COMMAND test_ee24_dma)
add_test(NAME ee24_big_chunk COMMAND test_ee24_big_chunk)
add_test(NAME ee24_forever COMMAND test_ee24_forever)
add_test(NAME ee24_bare COMMAND test_ee24_bare)
add_test(NAME ee24_bare_dma COMMAND test_ee24_bare_dma)
