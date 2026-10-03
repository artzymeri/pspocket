# recomp/common: ELF/SELF/PRX loader, NID database, manifest format, hashes (plan section 3, "Loader").

# Data files embedded at build time (regenerate with scripts/harvest_nids.py <rpcs3-dir>).
set(PSPOCKET_RECOMP_EMBEDDED_DATA nids syscalls)
set(embedded_sources)
foreach(name IN LISTS PSPOCKET_RECOMP_EMBEDDED_DATA)
    set(input ${CMAKE_CURRENT_SOURCE_DIR}/data/${name}.txt)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_${name}.cpp)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DSYMBOL=k_${name}_txt
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
        DEPENDS ${input} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
        COMMENT "Embedding recomp/common/data/${name}.txt"
        VERBATIM)
    list(APPEND embedded_sources ${output})
endforeach()

add_library(pspocket_recomp_common STATIC
    src/bytes.cpp
    src/elf.cpp
    src/guest_image.cpp
    src/hash.cpp
    src/inventory.cpp
    src/json_writer.cpp
    src/manifest.cpp
    src/nid.cpp
    src/ppu_module.cpp
    src/sce.cpp
    src/spu_scan.cpp
    src/strings.cpp
    src/toml.cpp
    ${embedded_sources})
target_include_directories(pspocket_recomp_common PUBLIC include PRIVATE src)
target_link_libraries(pspocket_recomp_common PUBLIC pspocket::core)
pspocket_set_warnings(pspocket_recomp_common)
add_library(pspocket::recomp_common ALIAS pspocket_recomp_common)
