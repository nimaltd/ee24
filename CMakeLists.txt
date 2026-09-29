cmake_minimum_required(VERSION 3.16)

project(ee24
    VERSION     4.0.0
    LANGUAGES   C
    DESCRIPTION "Driver for 24xx I2C EEPROMs, 24C01 to 24C512, on the STM32 HAL"
    HOMEPAGE_URL "https://github.com/nimaltd/ee24")

option(EE24_BUILD_TESTS "Build the host unit tests" OFF)

# ee24.h includes the platform main.h, which it does not ship, for the HAL's
# I2C and GPIO types. Point EE24_CONFIG_DIR at the folder holding it, normally
# Core/Inc in a CubeMX project. The tests supply a stub instead.
if(EE24_BUILD_TESTS AND NOT DEFINED EE24_CONFIG_DIR)
    set(EE24_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test/stub")
endif()

add_library(ee24 STATIC src/ee24.c)
add_library(nimaltd::ee24 ALIAS ee24)

target_include_directories(ee24 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

target_compile_features(ee24 PUBLIC c_std_11)

if(DEFINED EE24_CONFIG_DIR)
    target_include_directories(ee24 PUBLIC ${EE24_CONFIG_DIR})
endif()

if(EE24_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
