cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(boost_bench C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Common settings for both variants
function(configure_variant target)
    target_link_libraries(${target}
        pico_stdlib
        hardware_clocks
        hardware_pll
        hardware_vreg
        hardware_watchdog
        hardware_timer
        hardware_sync
        hardware_sha256
    )
    target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 1)   # also on GP0/GP1 at 115200 if you prefer
    pico_add_extra_outputs(${target})
endfunction()

# 1) Executes from flash over XIP. Exercises the QMI retiming path.
add_executable(boost_bench_flash /src/boost_bench.c)
configure_variant(boost_bench_flash)

# 2) Whole image copied to SRAM at boot. Flash timing cannot bite you here,
#    so this is the variant to try first if the flash build misbehaves.
add_executable(boost_bench_ram /src/boost_bench.c)
pico_set_binary_type(boost_bench_ram copy_to_ram)
configure_variant(boost_bench_ram)
