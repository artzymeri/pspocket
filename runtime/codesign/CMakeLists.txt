# runtime/codesign: code signing on the device for route B (D-147, D-150): a minimal Mach-O writer and a signer
# (CodeDirectory, requirements, CMS through the platform's crypto).

add_library(pspocket_codesign STATIC
    src/macho.cpp
    src/codesign.cpp)
if(APPLE)
    target_sources(pspocket_codesign PRIVATE src/identity_apple.cpp)
    target_link_libraries(pspocket_codesign PUBLIC "-framework Security" "-framework CoreFoundation")
endif()
target_include_directories(pspocket_codesign PUBLIC include)
target_link_libraries(pspocket_codesign PUBLIC pspocket::core PRIVATE pspocket::recomp_common)
pspocket_set_warnings(pspocket_codesign)
add_library(pspocket::codesign ALIAS pspocket_codesign)

# Host tool: makes and signs the go/no-go library on the Mac (with the keychain's development identity, or ad hoc).
if(APPLE)
    add_executable(pspocket-codesign tools/pspocket_codesign.cpp)
    target_link_libraries(pspocket-codesign PRIVATE pspocket::codesign)
    pspocket_set_warnings(pspocket-codesign)
endif()
