# Unity is vendored in test/unity. It is third party code, so it is built as its
# own target and is deliberately left out of this project's -Werror.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

set(EE24_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# The suite against the ee24_config.h users get, which has no RTOS.
add_executable(test_ee24 test_ee24.c)
target_link_libraries(test_ee24 PRIVATE ee24 unity)

# The stubs stand in for the CubeMX main.h and the HAL.
target_include_directories(test_ee24 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ee24 PRIVATE ${EE24_WARNINGS})
    target_compile_options(ee24 PRIVATE ${EE24_WARNINGS})
endif()

add_test(NAME ee24 COMMAND test_ee24)

# The same suite once for each RTOS, with ee24.c built into each. The setting
# cannot come from another config file: ee24.h includes ee24_config.h from its
# own folder, and that always wins over an include path. So these builds define
# the config's include guard, which turns that include into nothing, and give
# EE24_RTOS on the command line instead.
foreach(rtos CMSIS_V1 CMSIS_V2 THREADX)
    string(TOLOWER ${rtos} name)

    add_executable(test_ee24_${name} test_ee24.c ${PROJECT_SOURCE_DIR}/src/ee24.c)
    target_link_libraries(test_ee24_${name} PRIVATE unity)
    target_include_directories(test_ee24_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/stub)
    target_compile_definitions(test_ee24_${name} PRIVATE
        EE24_CONFIG_H
        EE24_RTOS=EE24_RTOS_${rtos})
    target_compile_features(test_ee24_${name} PRIVATE c_std_11)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_ee24_${name} PRIVATE ${EE24_WARNINGS})
    endif()

    add_test(NAME ee24_${name} COMMAND test_ee24_${name})
endforeach()
