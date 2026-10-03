# Pinned third-party code. Each entry records its version and licence in third_party/README.md.

if(PSPOCKET_BUILD_TESTS)
    # Catch2 v3.16.0, amalgamated (BSL-1.0)
    add_library(pspocket_catch2 STATIC catch2/catch_amalgamated.cpp)
    target_include_directories(pspocket_catch2 SYSTEM PUBLIC catch2)
    add_library(pspocket::catch2 ALIAS pspocket_catch2)
endif()
