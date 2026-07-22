cmake_minimum_required(VERSION 3.16)
project(adamcore C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Sources grow milestone by milestone; see README.
if(WIN32)
  set(ADAMCORE_NET_SOURCE src/net_win32.c)
else()
  set(ADAMCORE_NET_SOURCE src/net_posix.c)
endif()

add_library(adamcore STATIC
  src/z80.c
  src/tms9928a.c
  src/sn76489.c
  src/machine.c
  src/cart.c
  src/adamnet.c
  src/boip.c
  ${ADAMCORE_NET_SOURCE}
  src/palette.c
  src/debug.c
)
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

add_executable(debug_step_test tests/machine/debug_step_test.c)
target_include_directories(debug_step_test PRIVATE src)
target_link_libraries(debug_step_test adamcore)

add_test(NAME harte COMMAND harte_runner ${CMAKE_SOURCE_DIR}/tests/data/harte-z80)
add_test(NAME zexdoc COMMAND zex_runner ${CMAKE_SOURCE_DIR}/tests/data/zexdoc.com)
add_test(NAME boip COMMAND boip_fake_fujinet)
# Needs the (non-redistributable) system ROMs; skips when absent.
add_test(NAME debug_step COMMAND debug_step_test ${CMAKE_SOURCE_DIR}/tests/data/roms)
set_tests_properties(debug_step PROPERTIES SKIP_RETURN_CODE 77)
