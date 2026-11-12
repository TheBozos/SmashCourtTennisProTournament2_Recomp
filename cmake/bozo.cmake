# Smash Court Tennis Pro Tournament 2 on BozoEngine: the game's EE code recompiled by bozo-recomp, SCTPT2's bozo/port/
# on BozoEngine's port API, every Sony library native.
#
#   cmake -S . -B build/bozo -G Ninja -DSCT2_ELF=<SLUS_209.33> -DSCT2_DISC=<disc image> [-DBOZO_ENGINE_DIR=<BozoEngine>]
#         [-DBOZO_FFMPEG_ROOT=<dir>]
#
# Targets: sct2-headless (no window, BozoEngine's free host).

set(BOZO_ENGINE_DIR "" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)")
if(NOT BOZO_ENGINE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/BozoEngine/CMakeLists.txt")
    set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "BozoEngine's source tree (default: the BozoEngine submodule)" FORCE)
endif()
set(SCT2_ELF "" CACHE FILEPATH "SCTPT2's executable (SLUS_209.33), extracted from the disc")
set(SCT2_DISC "" CACHE FILEPATH "SCTPT2's disc image: its code overlays are unpacked from it")
set(SCT2_GEN_FILES 32 CACHE STRING "How many .cpp files the generated code is split into")
set(SCT2_GEN_OPT "-O1" CACHE STRING "Optimisation of the generated code")
if(NOT BOZO_ENGINE_DIR OR NOT EXISTS "${BOZO_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SCTPT2: set BOZO_ENGINE_DIR to BozoEngine's source tree")
endif()
if(NOT SCT2_ELF OR NOT EXISTS "${SCT2_ELF}")
    message(FATAL_ERROR "SCTPT2: set SCT2_ELF to the game's executable (SLUS_209.33)")
endif()
if(NOT SCT2_DISC OR NOT EXISTS "${SCT2_DISC}")
    message(FATAL_ERROR "SCTPT2: set SCT2_DISC to the game's disc image")
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

set(ovl_dir "${gen_dir}/overlays")
set(sct2_modules boot:1 ovl_213932:2 ovl_21381a:8 ovl_2138a8:16)
set(ovl_outputs ${ovl_dir}/main.elf ${ovl_dir}/sct2_overlays.cpp)
foreach(spec ${sct2_modules})
    string(REPLACE ":" ";" spec "${spec}")
    list(GET spec 0 module)
    list(APPEND ovl_outputs ${ovl_dir}/${module}.elf ${ovl_dir}/${module}.entries)
endforeach()
add_custom_command(
    OUTPUT ${ovl_outputs}
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" ${SCT2_ELF} ${SCT2_DISC} ${ovl_dir}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" "${CMAKE_SOURCE_DIR}/tools/sct2_irx_extract.py" ${SCT2_ELF}
    COMMENT "Unpacking SCTPT2's code overlays")

# bozo-recomp's outputs for a prefix split into a number of files.
function(sct2_gen_files prefix dir count out)
    set(files ${dir}/${prefix}_functions.h ${dir}/${prefix}_table.cpp)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(LENGTH "${i}" len)
        if(len EQUAL 1)
            set(n "00${i}")
        elseif(len EQUAL 2)
            set(n "0${i}")
        else()
            set(n "${i}")
        endif()
        list(APPEND files ${dir}/${prefix}_${n}.cpp)
    endforeach()
    set(${out} ${files} PARENT_SCOPE)
endfunction()

set(main_externals "${gen_dir}/sct2_main_externals.txt")
add_custom_command(
    OUTPUT ${main_externals}
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" --externals ${externals} ${overrides_list}
            ${main_externals}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" ${externals} ${overrides_list}
    COMMENT "Writing the main program's externals")
sct2_gen_files(sct2 ${gen_dir} ${SCT2_GEN_FILES} gen_files)
add_custom_command(
    OUTPUT ${gen_files}
    COMMAND bozo-recomp ${ovl_dir}/main.elf ${gen_dir} --prefix sct2 --files ${SCT2_GEN_FILES} --no-comments
            --externals ${main_externals} --symbols ${SCT2_SYMBOLS}
    DEPENDS bozo-recomp ${main_externals} ${SCT2_SYMBOLS} ${ovl_dir}/main.elf
    COMMENT "Recompiling SCTPT2")
foreach(spec ${sct2_modules})
    string(REPLACE ":" ";" spec "${spec}")
    list(GET spec 0 module)
    list(GET spec 1 count)
    set(dir ${ovl_dir}/${module})
    sct2_gen_files(sct2_${module} ${dir} ${count} files)
    add_custom_command(
        OUTPUT ${files}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
        COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" --externals ${externals}
                ${overrides_list} ${dir}.externals ${module}
        COMMAND bozo-recomp ${ovl_dir}/${module}.elf ${dir} --prefix sct2_${module} --symbol-prefix ${module}_ --files ${count} --no-comments
                --externals ${dir}.externals --entries ${ovl_dir}/${module}.entries
        DEPENDS bozo-recomp "${CMAKE_SOURCE_DIR}/tools/sct2_overlays.py" ${externals} ${overrides_list}
                ${ovl_dir}/${module}.elf ${ovl_dir}/${module}.entries
        COMMENT "Recompiling SCTPT2's ${module}")
    list(APPEND gen_files ${files})
endforeach()
list(APPEND gen_files ${ovl_dir}/sct2_overlays.cpp)
add_custom_target(sct2_recompile DEPENDS ${gen_files})
add_library(sct2_generated OBJECT ${gen_files})
target_link_libraries(sct2_generated PUBLIC bozo_runtime_headers)
target_include_directories(sct2_generated PUBLIC ${gen_dir})
target_compile_options(sct2_generated PRIVATE ${SCT2_GEN_OPT} -w)

set(irx_dir "${gen_dir}/iop")
set(native_irx "${BOZO_ENGINE_DIR}/tools/native_irx")
set(irx_modules sio2man multitap_manager padman Sound_Device_Library Midi_Sequencer_Module SPU2_Synthesizer_Module
                Midi_Message_Input_Module SE_Sequence_Input_Module mcman mcserv PS2LIB_Filectrl PS2LIB_Sound)
list(TRANSFORM irx_modules PREPEND "${irx_dir}/raw/" OUTPUT_VARIABLE irx_raw)
list(TRANSFORM irx_raw APPEND ".IRX")
add_custom_command(
    OUTPUT ${irx_raw}
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_irx_extract.py" ${SCT2_ELF} ${irx_dir}/raw
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/sct2_irx_extract.py" ${SCT2_ELF}
    COMMENT "Unpacking SCTPT2's IOP modules")
set(irx_sources)
set(irx_images)
foreach(spec "Sound_Device_Library:0x8000:libsd" "Midi_Sequencer_Module:0x10000:" "SPU2_Synthesizer_Module:0x16000:"
             "Midi_Message_Input_Module:0x26000:" "SE_Sequence_Input_Module:0x27000:" "PS2LIB_Filectrl:0x29000:"
             "PS2LIB_Sound:0x2c000:")
    string(REPLACE ":" ";" spec "${spec}")
    list(GET spec 0 module)
    list(GET spec 1 base)
    list(GET spec 2 flag)
    set(irx ${irx_dir}/raw/${module}.IRX)
    string(TOLOWER ${module} stem)
    add_custom_command(
        OUTPUT ${irx_dir}/${module}.json ${irx_dir}/${module}.bin ${irx_dir}/${stem}_iop.cpp
        COMMAND Python3::Interpreter -I "${native_irx}/relocate.py" ${irx} ${base} ${irx_dir}/${module}
        COMMAND Python3::Interpreter -I "${native_irx}/recompile.py" ${irx_dir}/${module}.json --out ${irx_dir}
        DEPENDS ${irx} "${native_irx}/relocate.py" "${native_irx}/recompile.py"
        COMMENT "Compiling ${module}")
    list(APPEND irx_sources ${irx_dir}/${stem}_iop.cpp)
    if(flag)
        list(APPEND irx_images ${irx_dir}/${module}.json:${flag})
    else()
        list(APPEND irx_images ${irx_dir}/${module}.json)
    endif()
endforeach()
add_custom_command(
    OUTPUT ${irx_dir}/sct2_irx_images.cpp
    COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/sct2_irx_images.py" "${native_irx}" ${irx_dir}/sct2_irx_images.cpp ${irx_images}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/sct2_irx_images.py" ${irx_sources}
    COMMENT "Writing SCTPT2's IOP module images")
add_library(sct2_iop OBJECT ${irx_sources} ${irx_dir}/sct2_irx_images.cpp)
target_include_directories(sct2_iop PRIVATE "${CMAKE_SOURCE_DIR}/bozo/port")
target_link_libraries(sct2_iop PUBLIC bozo_runtime_headers bozo_libs)
target_compile_options(sct2_iop PRIVATE -w)

add_library(sct2_port OBJECT ${SCT2_PORT_SOURCES})
target_include_directories(sct2_port PUBLIC "${CMAKE_SOURCE_DIR}/bozo/port" ${gen_dir})
target_link_libraries(sct2_port PUBLIC bozo_port bozo_render bozo_libs bozo_runtime_core)
add_dependencies(sct2_port sct2_generated)

add_executable(sct2-headless bozo/sct2_headless.cpp)
target_link_libraries(sct2-headless PRIVATE sct2_port sct2_generated sct2_iop bozo_host)
target_compile_definitions(sct2-headless PRIVATE BOZO_RUN_DEFAULT_ELF="${SCT2_ELF}"
                                                  BOZO_RUN_DEFAULT_SYMBOLS="${SCT2_SYMBOLS}")
