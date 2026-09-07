cmake_minimum_required(VERSION 3.16)
project(adamcore C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# The source list lives in adamcore-sources.cmake and is used from there by
# this build AND by the hosts that stage these sources into their own tree, so
# adding a file here cannot silently break their link.
include(${CMAKE_CURRENT_LIST_DIR}/adamcore-sources.cmake)
adamcore_sources(ADAMCORE_SOURCES ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_library(adamcore STATIC ${ADAMCORE_SOURCES})
target_include_directories(adamcore PUBLIC include PRIVATE src)
set_property(TARGET adamcore PROPERTY POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_link_libraries(adamcore PUBLIC ws2_32)
endif()

add_executable(adamcore-cli cli/adamcore-cli.c)
target_link_libraries(adamcore-cli adamcore)

add_executable(eos_trace tests/machine/eos_trace.c)
target_include_directories(eos_trace PRIVATE src)
target_link_libraries(eos_trace adamcore)

enable_testing()

add_executable(harte_runner tests/z80/harte_runner.c tests/vendor/cJSON.c)
target_include_directories(harte_runner PRIVATE src tests/vendor)
target_link_libraries(harte_runner adamcore)

add_executable(zex_runner tests/z80/zex_runner.c)
target_include_directories(zex_runner PRIVATE src)
target_link_libraries(zex_runner adamcore)

add_executable(boip_fake_fujinet tests/adamnet/boip_fake_fujinet.c)
target_include_directories(boip_fake_fujinet PRIVATE src)
target_link_libraries(boip_fake_fujinet adamcore pthread)

add_executable(cart_ops_test tests/machine/cart_ops_test.c)
target_link_libraries(cart_ops_test adamcore)
add_test(NAME cart_ops COMMAND cart_ops_test)

add_executable(sgm_test tests/machine/sgm_test.c)
target_link_libraries(sgm_test adamcore)
add_test(NAME sgm COMMAND sgm_test)

add_executable(psg_test tests/psg/psg_test.c)
target_link_libraries(psg_test adamcore)
# Captured from the pre-refactor renderer (psg_test --capture). The split of
# the shared timeline out of the chip model must not change a single sample
# for a machine with no Super Game Module.
target_compile_definitions(psg_test PRIVATE GOLDEN_SUM=0x5c0921643042f3c0ULL)
add_test(NAME psg COMMAND psg_test)

add_executable(debug_step_test tests/machine/debug_step_test.c)
target_include_directories(debug_step_test PRIVATE src)
target_link_libraries(debug_step_test adamcore)

add_test(NAME harte COMMAND harte_runner ${CMAKE_SOURCE_DIR}/tests/data/harte-z80)
add_test(NAME zexdoc COMMAND zex_runner ${CMAKE_SOURCE_DIR}/tests/data/zexdoc.com)
add_test(NAME boip COMMAND boip_fake_fujinet)
# Needs the (non-redistributable) system ROMs; skips when absent.
add_test(NAME debug_step COMMAND debug_step_test ${CMAKE_SOURCE_DIR}/tests/data/roms)
set_tests_properties(debug_step PROPERTIES SKIP_RETURN_CODE 77)
