# Smash Court Tennis Pro Tournament 2 on BozoEngine: the game's EE code recompiled by bozo-recomp, SCTPT2's bozo/port/
# on BozoEngine's port API, every Sony library native.
#
#   cmake -S . -B build/bozo -G Ninja -DSCT2_ELF=<SLUS_209.33> [-DBOZO_ENGINE_DIR=<BozoEngine>] [-DBOZO_FFMPEG_ROOT=<dir>]
#
# Targets: sct2-headless (no window, BozoEngine's free host).

set(BOZO_ENGINE_DIR "" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)")
if(NOT BOZO_ENGINE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/BozoEngine/CMakeLists.txt")
    set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)" FORCE)
endif()
set(SCT2_ELF "" CACHE FILEPATH "SCTPT2's executable (SLUS_209.33), extracted from the disc")
set(SCT2_GEN_FILES 32 CACHE STRING "How many .cpp files the generated code is split into")
set(SCT2_GEN_OPT "-O1" CACHE STRING "Optimisation of the generated code")
if(NOT BOZO_ENGINE_DIR OR NOT EXISTS "${BOZO_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SCTPT2: set BOZO_ENGINE_DIR to BozoEngine's source tree")
endif()
if(NOT SCT2_ELF OR NOT EXISTS "${SCT2_ELF}")
    message(FATAL_ERROR "SCTPT2: set SCT2_ELF to the game's executable (SLUS_209.33)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BOZO_TMB_ELF "" CACHE FILEPATH "" FORCE)
set(BOZO_APP OFF CACHE BOOL "" FORCE)
add_subdirectory("${BOZO_ENGINE_DIR}" bozo)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(gen_dir "${CMAKE_BINARY_DIR}/sct2_gen")
set(overrides_list "${gen_dir}/sct2_overrides.txt")
set(externals "${gen_dir}/sct2_externals.txt")
set(SCT2_SYMBOLS "${CMAKE_SOURCE_DIR}/recomp/sct2_symbols.txt")
file(GLOB SCT2_PORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/bozo/port/*.cpp")

add_custom_command(
    OUTPUT ${overrides_list}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_overrides_list.py" "${CMAKE_SOURCE_DIR}/bozo/port" ${overrides_list}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/sct2_overrides_list.py" ${SCT2_PORT_SOURCES}
    COMMENT "Listing SCTPT2's overrides")
add_custom_command(
    OUTPUT ${externals}
    COMMAND bozo-externals ${SCT2_ELF} ${externals} --symbols ${SCT2_SYMBOLS} --overrides ${overrides_list}
    DEPENDS bozo-externals ${overrides_list} ${SCT2_SYMBOLS} ${SCT2_ELF}
    COMMENT "Writing the externals list")

set(gen_files ${gen_dir}/sct2_functions.h ${gen_dir}/sct2_table.cpp)
math(EXPR last "${SCT2_GEN_FILES} - 1")
foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
        set(n "00${i}")
    elseif(len EQUAL 2)
        set(n "0${i}")
    else()
        set(n "${i}")
    endif()
    list(APPEND gen_files ${gen_dir}/sct2_${n}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${gen_files}
    COMMAND bozo-recomp ${SCT2_ELF} ${gen_dir} --prefix sct2 --files ${SCT2_GEN_FILES} --no-comments --externals ${externals}
            --symbols ${SCT2_SYMBOLS}
    DEPENDS bozo-recomp ${externals} ${SCT2_SYMBOLS} ${SCT2_ELF}
    COMMENT "Recompiling SCTPT2")
add_custom_target(sct2_recompile DEPENDS ${gen_files})
add_library(sct2_generated OBJECT ${gen_files})
target_link_libraries(sct2_generated PUBLIC bozo_runtime_headers)
target_include_directories(sct2_generated PUBLIC ${gen_dir})
target_compile_options(sct2_generated PRIVATE ${SCT2_GEN_OPT} -w)

add_library(sct2_port OBJECT ${SCT2_PORT_SOURCES})
target_include_directories(sct2_port PUBLIC "${CMAKE_SOURCE_DIR}/bozo/port" ${gen_dir})
target_link_libraries(sct2_port PUBLIC bozo_port bozo_render bozo_libs bozo_runtime_core)
add_dependencies(sct2_port sct2_generated)

add_executable(sct2-headless bozo/sct2_headless.cpp)
target_link_libraries(sct2-headless PRIVATE sct2_port sct2_generated bozo_host)
target_compile_definitions(sct2-headless PRIVATE BOZO_RUN_DEFAULT_ELF="${SCT2_ELF}"
                                                  BOZO_RUN_DEFAULT_SYMBOLS="${SCT2_SYMBOLS}")
