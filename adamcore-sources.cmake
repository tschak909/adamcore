# The list of adamcore translation units, shipped so that consumers which
# compile the sources themselves do not have to keep their own copy.
#
# Hosts in the FujiNet Go family stage adamcore's src/ and include/ into their
# own tree and build them with their own flags (see fujinet-go-adam-desktop's
# cmake/StageAdamcore.cmake). Before this file each of them enumerated the .c
# files by hand, which meant every new source added here -- psg.c and ay8910.c
# most recently -- silently broke their link until each was edited separately.
#
# Usage from a consumer, after staging into ${GEN}:
#
#     include("${GEN}/adamcore-sources.cmake")
#     adamcore_sources(MY_SOURCES ROOT "${GEN}")
#     add_library(mycore STATIC ${MY_SOURCES} ...)
#
# ROOT defaults to this file's own directory, which is the right answer both
# in this repository and in a staged copy.

function(adamcore_sources out_var)
  cmake_parse_arguments(ACS "" "ROOT" "" ${ARGN})
  if(NOT ACS_ROOT)
    set(ACS_ROOT "${CMAKE_CURRENT_LIST_DIR}")
  endif()

  set(_srcs
    src/z80.c
    src/tms9928a.c
    src/sn76489.c
    src/ay8910.c
    src/psg.c
    src/machine.c
    src/cart.c
    src/adamnet.c
    src/boip.c
    src/palette.c
    src/debug.c)

  # The socket helpers behind net.h: POSIX everywhere, Winsock on Windows.
  if(WIN32)
    list(APPEND _srcs src/net_win32.c)
  else()
    list(APPEND _srcs src/net_posix.c)
  endif()

  set(_out "")
  foreach(_s IN LISTS _srcs)
    if(NOT EXISTS "${ACS_ROOT}/${_s}")
      message(FATAL_ERROR
        "adamcore_sources: ${ACS_ROOT}/${_s} is missing. If this is a staged "
        "copy, re-stage it; the staged tree is older than this file.")
    endif()
    list(APPEND _out "${ACS_ROOT}/${_s}")
  endforeach()
  set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()
