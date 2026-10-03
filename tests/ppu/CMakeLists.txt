# PPU tests: interpreter unit vectors, analyzer/lifter tests, differential fuzz, program-level tests.

# ---- unit tests (hand-checked vectors, decoder, disassembler, assembler, analyzer)
pspocket_add_test(test_ppu
    SOURCES test_decoder.cpp test_interp_int.cpp test_interp_branch_mem.cpp test_interp_fp.cpp test_analyzer.cpp
        test_dispatch.cpp
    LIBS pspocket::recomp_ppu)
target_include_directories(test_ppu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- differential fuzz: interpreter vs lifted code, every supported instruction form
set(PSPOCKET_PPU_FUZZ_CASES 256 CACHE STRING "Differential fuzz cases per PPU instruction form (at least 200)")
set(PSPOCKET_PPU_FUZZ_TUS 32 CACHE STRING "Translation units the lifted fuzz cases are split into")

add_executable(ppu-fuzz-gen fuzz_gen.cpp)
target_include_directories(ppu-fuzz-gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ppu-fuzz-gen PRIVATE pspocket::recomp_ppu)
pspocket_set_warnings(ppu-fuzz-gen)

set(fuzz_dir ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
set(fuzz_sources ${fuzz_dir}/fuzz_cases.c)
math(EXPR fuzz_last "${PSPOCKET_PPU_FUZZ_TUS} - 1")
foreach(i RANGE ${fuzz_last})
    list(APPEND fuzz_sources ${fuzz_dir}/fuzz_lifted_${i}.c)
endforeach()
add_custom_command(
    OUTPUT ${fuzz_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${fuzz_dir}
    COMMAND ppu-fuzz-gen --out ${fuzz_dir} --cases ${PSPOCKET_PPU_FUZZ_CASES} --tus ${PSPOCKET_PPU_FUZZ_TUS}
    DEPENDS ppu-fuzz-gen
    COMMENT "Generating the PPU differential fuzz cases"
    VERBATIM)
add_library(ppu_fuzz_generated OBJECT ${fuzz_sources})
target_include_directories(ppu_fuzz_generated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ppu_fuzz_generated PRIVATE pspocket::ppu)
pspocket_set_warnings(ppu_fuzz_generated)
pspocket_set_generated_warnings(ppu_fuzz_generated)

pspocket_add_test(test_ppu_fuzz SOURCES test_fuzz.cpp $<TARGET_OBJECTS:ppu_fuzz_generated> LIBS pspocket::ppu)
target_include_directories(test_ppu_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- program-level tests: synthetic programs lifted by ppu-lift at build time
add_executable(ppu-progs-gen progs_gen.cpp)
target_link_libraries(ppu-progs-gen PRIVATE pspocket::recomp_ppu)
pspocket_set_warnings(ppu-progs-gen)

set(progs_dir ${CMAKE_CURRENT_BINARY_DIR}/progs)
set(progs_tus 4)
set(progs_sources ${progs_dir}/ppu_progs.h ${progs_dir}/ppu_progs_reg.c)
math(EXPR progs_last "${progs_tus} - 1")
foreach(i RANGE ${progs_last})
    list(APPEND progs_sources ${progs_dir}/ppu_progs_${i}.c)
endforeach()
add_custom_command(
    OUTPUT ${progs_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${progs_dir}
    COMMAND ppu-progs-gen --bin ${progs_dir}/progs.bin --seeds ${progs_dir}/progs.seeds
    COMMAND ppu-lift --image ${progs_dir}/progs.bin --base 0x00100000 --seeds-file ${progs_dir}/progs.seeds
            --out ${progs_dir} --tu ${progs_tus} --name ppu_progs --prefix ppu_progs_ --quiet
    DEPENDS ppu-progs-gen ppu-lift
    COMMENT "Lifting the PPU test programs"
    VERBATIM)
add_library(ppu_progs_generated OBJECT ${progs_sources})
target_link_libraries(ppu_progs_generated PRIVATE pspocket::ppu)
pspocket_set_warnings(ppu_progs_generated)
pspocket_set_generated_warnings(ppu_progs_generated)

pspocket_add_test(test_ppu_programs SOURCES test_programs.cpp $<TARGET_OBJECTS:ppu_progs_generated>
    LIBS pspocket::recomp_ppu)
target_include_directories(test_ppu_programs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${progs_dir})
