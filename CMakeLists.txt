cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set. Run scripts/bootstrap-host.sh")
  endif()
endif()

project(galaxyvita C)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "GalaxyVita")
set(VITA_TITLEID "GVITA0001")
set(VITA_VERSION "00.01")

# ATTRIBUTE2 bit 0xC = +109 MiB enlarged user memory. Harmless on the hello
# binary; mandatory once the AOT module + 88 MiB guest RAM are linked.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

add_executable(galaxyvita
  vita/host/main.c
)

target_link_libraries(galaxyvita
  SceDisplay_stub
  SceCtrl_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceSysmem_stub
  SceIofilemgr_stub
)

vita_create_self(eboot.bin galaxyvita)
vita_create_vpk(galaxyvita.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
