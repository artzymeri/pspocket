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

# The SELF keys the build carries (D-121), read from RPCS3's key_vault.cpp into the build directory only: keys never go
# into the repository (CLAUDE.md rule 1). Without an RPCS3 source tree the table is empty and decrypt_self says so.
set(PSPOCKET_RPCS3_DIR_DEFAULT "")
if(EXISTS ${CMAKE_SOURCE_DIR}/external/rpcs3/rpcs3/Crypto/key_vault.cpp)
    set(PSPOCKET_RPCS3_DIR_DEFAULT ${CMAKE_SOURCE_DIR}/external/rpcs3) # inside pspocket, gitignored
endif()
set(PSPOCKET_RPCS3_DIR "${PSPOCKET_RPCS3_DIR_DEFAULT}" CACHE PATH "RPCS3 source tree for the SELF keys (D-121)")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(self_keys_output ${CMAKE_CURRENT_BINARY_DIR}/generated/self_keys.cpp)
set(self_keys_depends ${CMAKE_SOURCE_DIR}/scripts/self-keys.py)
foreach(vault ${PSPOCKET_RPCS3_DIR}/rpcs3/Crypto/key_vault.cpp ${PSPOCKET_RPCS3_DIR}/Crypto/key_vault.cpp
        ${PSPOCKET_RPCS3_DIR}/rpcs3/Crypto/key_vault.h ${PSPOCKET_RPCS3_DIR}/Crypto/key_vault.h)
    if(PSPOCKET_RPCS3_DIR AND EXISTS ${vault})
        list(APPEND self_keys_depends ${vault})
    endif()
endforeach()
add_custom_command(
    OUTPUT ${self_keys_output}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/self-keys.py "${PSPOCKET_RPCS3_DIR}" ${self_keys_output}
    DEPENDS ${self_keys_depends}
    COMMENT "SELF keys (scripts/self-keys.py)"
    VERBATIM)
find_package(ZLIB)

add_library(pspocket_recomp_common STATIC
    src/bytes.cpp
    src/elf.cpp
    src/guest_image.cpp
    src/hash.cpp
    src/inventory.cpp
    src/json_writer.cpp
    src/manifest.cpp
    src/nid.cpp
    src/npd.cpp
    src/ppu_module.cpp
    src/prx_place.cpp
    src/sce.cpp
    src/self_decrypt.cpp
    src/firmware.cpp
    src/spu_scan.cpp
    src/strings.cpp
    src/toml.cpp
    ${embedded_sources}
    ${self_keys_output})
target_include_directories(pspocket_recomp_common PUBLIC include PRIVATE src)
target_link_libraries(pspocket_recomp_common PUBLIC pspocket::core)
if(ZLIB_FOUND)
    target_link_libraries(pspocket_recomp_common PUBLIC ZLIB::ZLIB) # SELF segments are zlib (self_decrypt.cpp)
endif()
pspocket_set_warnings(pspocket_recomp_common)
add_library(pspocket::recomp_common ALIAS pspocket_recomp_common)

# Host tool: /dev_flash from a PS3UPDAT.PUP, as the phone installs it (D-156).
add_executable(pspocket-firmware tools/pspocket_firmware.cpp)
target_link_libraries(pspocket-firmware PRIVATE pspocket::recomp_common)
pspocket_set_warnings(pspocket-firmware)
