add_library(pspocket_selftest STATIC
    src/selftest.cpp
    src/probes_core.cpp
    src/probes_ppu.cpp
    src/probes_codegen.cpp)
target_include_directories(pspocket_selftest PUBLIC include PRIVATE src)
target_link_libraries(pspocket_selftest PUBLIC pspocket::core PRIVATE pspocket::codesign)
if(TARGET pspocket_codegen) # route B's compiler (D-151), with an LLVM build
    target_link_libraries(pspocket_selftest PRIVATE pspocket::codegen)
    target_compile_definitions(pspocket_selftest PRIVATE PSPOCKET_HAVE_CODEGEN=1
        PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
target_compile_definitions(pspocket_selftest PRIVATE PSPOCKET_VERSION="${PROJECT_VERSION}")
pspocket_set_warnings(pspocket_selftest)
add_library(pspocket::selftest ALIAS pspocket_selftest)

# PPU probes (ppu.interp, ppu.lifted). Their benchmark is generated at build time by recomp/ppu into
# ${CMAKE_BINARY_DIR}/generated/selftest/ with stable names; the iOS app compiles the same .c files from the host
# build (app/ios/project.yml). The raw image and seeds stay in generated/selftest-work/.
pspocket_component_enabled(recomp/ppu selftest_recomp_ppu_enabled)
if(TARGET pspocket_ppu AND selftest_recomp_ppu_enabled AND EXISTS ${PROJECT_SOURCE_DIR}/recomp/ppu/CMakeLists.txt)
    set(selftest_gen_dir ${CMAKE_BINARY_DIR}/generated/selftest)
    set(selftest_work_dir ${CMAKE_BINARY_DIR}/generated/selftest-work)
    set(selftest_ppu_sources
        ${selftest_gen_dir}/ps_selftest_ppu_image.c
        ${selftest_gen_dir}/ps_selftest_ppu_0.c
        ${selftest_gen_dir}/ps_selftest_ppu_reg.c)
    set(selftest_ppu_headers
        ${selftest_gen_dir}/ps_selftest_ppu_image.h
        ${selftest_gen_dir}/ps_selftest_ppu.h)
    # ppu-selftest-gen and ppu-lift are defined later (recomp/ppu); target names resolve at generate time.
    add_custom_command(
        OUTPUT ${selftest_ppu_sources} ${selftest_ppu_headers}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${selftest_gen_dir} ${selftest_work_dir}
        COMMAND ppu-selftest-gen --bin ${selftest_work_dir}/ps_selftest_ppu.bin
                --seeds ${selftest_work_dir}/ps_selftest_ppu.seeds
                --c ${selftest_gen_dir}/ps_selftest_ppu_image.c --h ${selftest_gen_dir}/ps_selftest_ppu_image.h
        COMMAND ppu-lift --image ${selftest_work_dir}/ps_selftest_ppu.bin --base 0x00010000
                --seeds-file ${selftest_work_dir}/ps_selftest_ppu.seeds --out ${selftest_gen_dir}
                --name ps_selftest_ppu --prefix ps_selftest_ppu_ --tu 1 --quiet
        DEPENDS ppu-selftest-gen ppu-lift
        COMMENT "Generating the lifted PPU self-test benchmark"
        VERBATIM)
    add_library(pspocket_selftest_ppu_generated OBJECT ${selftest_ppu_sources})
    target_link_libraries(pspocket_selftest_ppu_generated PRIVATE pspocket::ppu)
    pspocket_set_warnings(pspocket_selftest_ppu_generated)
    pspocket_set_generated_warnings(pspocket_selftest_ppu_generated)

    target_sources(pspocket_selftest PRIVATE $<TARGET_OBJECTS:pspocket_selftest_ppu_generated> ${selftest_ppu_headers})
    target_include_directories(pspocket_selftest PRIVATE ${selftest_gen_dir})
    target_link_libraries(pspocket_selftest PUBLIC pspocket::ppu)
    target_compile_definitions(pspocket_selftest PRIVATE PSPOCKET_SELFTEST_PPU_REQUIRED=1)
else()
    target_compile_definitions(pspocket_selftest PRIVATE PSPOCKET_SELFTEST_PPU_DISABLED=1)
endif()
