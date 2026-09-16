cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set. Run scripts/bootstrap-host.sh")
  endif()
endif()

project(galaxyvita C CXX)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "GalaxyVita")
set(VITA_TITLEID "GVITA0001")
set(VITA_VERSION "00.05")

# ATTRIBUTE2 bit 0xC = +109 MiB enlarged user memory.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

set(DOLRECOMP_SRC "${CMAKE_SOURCE_DIR}/third_party/DolRecomp/src")
set(MODERNGEKKO "${CMAKE_SOURCE_DIR}/third_party/ModernGekko")
set(AOT_GEN_DIR "${CMAKE_SOURCE_DIR}/generated/aot/generated")
set(AOT_OBJ_DIR "${CMAKE_SOURCE_DIR}/generated/build/aot-vita")

file(GLOB_RECURSE GALAXYVITA_AOT_OBJS "${AOT_OBJ_DIR}/*.obj")
if(GALAXYVITA_AOT_OBJS)
  list(FILTER GALAXYVITA_AOT_OBJS EXCLUDE REGEX "CMakeCCompiler")
endif()

set(GALAXYVITA_AOT_SOURCES "")
if(NOT GALAXYVITA_AOT_OBJS)
  file(GLOB GALAXYVITA_AOT_SOURCES "${AOT_GEN_DIR}/*.c" "${AOT_GEN_DIR}/chunks/*.c")
endif()

list(LENGTH GALAXYVITA_AOT_OBJS _aot_n)
list(LENGTH GALAXYVITA_AOT_SOURCES _aot_src_n)
message(STATUS "AOT prebuilt objects: ${_aot_n}; AOT sources: ${_aot_src_n}")
if(_aot_n EQUAL 0 AND _aot_src_n EQUAL 0)
  message(FATAL_ERROR "No AOT objects or sources. Run ./scripts/generate-aot.sh and ./scripts/measure-aot-size.sh")
endif()

add_executable(galaxyvita
  vita/host/main.c
  vita/gfx/present.c
  runtime/chassis.c
  runtime/flipper.c
  runtime/gx_cp.cpp
  # ModernGekko's GPU stack: the FIFO decoder, the register files and the
  # vertex loader are Dolphin's, so only the glue in gx_cp.cpp is ours.
  ${MODERNGEKKO}/src/memory/address_space.cpp
  ${MODERNGEKKO}/src/gpu/gx_command_processor.cpp
  ${MODERNGEKKO}/src/gpu/gx_state_backend.cpp
  ${MODERNGEKKO}/src/gpu/gx_vertex_loader.cpp
  runtime/ios.c
  runtime/bt.c
  runtime/disc.c
  runtime/nand.c
  ${DOLRECOMP_SRC}/cpu/cpu.c
  ${DOLRECOMP_SRC}/frontend/container/dol.c
  ${GALAXYVITA_AOT_SOURCES}
)

if(GALAXYVITA_AOT_OBJS)
  set_source_files_properties(${GALAXYVITA_AOT_OBJS} PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE)
  target_sources(galaxyvita PRIVATE ${GALAXYVITA_AOT_OBJS})
endif()

target_include_directories(galaxyvita PRIVATE
  runtime
  vita/gfx
  ${DOLRECOMP_SRC}
  ${AOT_GEN_DIR}
  ${MODERNGEKKO}/include
  ${MODERNGEKKO}/vendor/dolphin_legacy
  ${MODERNGEKKO}/vendor/picojson
  ${MODERNGEKKO}/src/dolphin_shims
  ${MODERNGEKKO}/vendor/fmt/fmt/include
)
target_compile_definitions(galaxyvita PRIVATE FMT_HEADER_ONLY=1)
set_property(TARGET galaxyvita PROPERTY CXX_STANDARD 23)
set_property(TARGET galaxyvita PROPERTY CXX_STANDARD_REQUIRED ON)

target_compile_options(galaxyvita PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
)

target_link_libraries(galaxyvita
  m
  vitaGL
  mathneon
  vitashark
  SceShaccCgExt
  SceShaccCg_stub
  taihen_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceSysmodule_stub
  SceAppMgr_stub
  SceKernelDmacMgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceSysmem_stub
  SceIofilemgr_stub
  stdc++
)

set_target_properties(galaxyvita PROPERTIES LINKER_LANGUAGE CXX)

vita_create_self(eboot.bin galaxyvita)
vita_create_vpk(galaxyvita.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
