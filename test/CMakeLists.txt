# Unity is vendored in test/unity. It is third party code, so it is built as its
# own target and is deliberately left out of this project's -Werror.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

set(EE24_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# One build covers everything. ee24 reaches the RTOS only through osal, and the
# fake osal in fake/ lets the tests hold, refuse or delay the mutex at will, so
# there is no build per RTOS: those are osal's tests to run.
add_executable(test_ee24 test_ee24.c)
target_link_libraries(test_ee24 PRIVATE ee24 unity)

# The stubs stand in for the CubeMX main.h and the HAL.
target_include_directories(test_ee24 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ee24 PRIVATE ${EE24_WARNINGS})
    target_compile_options(ee24 PRIVATE ${EE24_WARNINGS})
endif()

add_test(NAME ee24 COMMAND test_ee24)
