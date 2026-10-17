# RSX front end (FIFO, methods, state, shader microcode, texture layouts, write tracking): portable C++.
# The iOS app compiles every source in src/ (D-014), so nothing here may be host-only or generated at configure time.
add_library(pspocket_rsx STATIC
    src/context.cpp
    src/index_scan.cpp
    src/io_map.cpp
    src/method_names.cpp
    src/methods.cpp
    src/null_backend.cpp
    src/rsx.cpp
    src/capture.cpp
    src/shader/fp.cpp
    src/shader/interp.cpp
    src/shader/msl.cpp
    src/shader/vp.cpp
    src/state.cpp
    src/texture.cpp
    src/write_tracker.cpp)
target_include_directories(pspocket_rsx PUBLIC include)
target_link_libraries(pspocket_rsx PUBLIC pspocket::core)
pspocket_set_warnings(pspocket_rsx)
add_library(pspocket::rsx ALIAS pspocket_rsx)

# Metal backend (Objective-C++ with ARC). Apple platforms only; the iOS build compiles metal/src/*.mm too and must
# enable ARC (CLANG_ENABLE_OBJC_ARC) and link Metal.framework.
if(APPLE)
    enable_language(OBJCXX)
    add_library(pspocket_rsx_metal STATIC
        metal/src/edge_geometry.mm
        metal/src/metal_backend.mm
        metal/src/pipeline_cache.mm
        metal/src/surface_cache.mm
        metal/src/shader_cache.mm
        metal/src/async_compiler.mm
        metal/src/texture_cache.mm)
    target_include_directories(pspocket_rsx_metal PUBLIC metal/include)
    target_link_libraries(pspocket_rsx_metal PUBLIC pspocket::rsx "-framework Metal" "-framework Foundation")
    target_compile_options(pspocket_rsx_metal PRIVATE -fobjc-arc)
    set_target_properties(pspocket_rsx_metal PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF)
    pspocket_set_warnings(pspocket_rsx_metal)
    add_library(pspocket::rsx_metal ALIAS pspocket_rsx_metal)
    # An RSX capture replayed through the Metal backend alone (D-1053).
    add_executable(rsx-replay metal/tools/rsx_replay.mm)
    target_link_libraries(rsx-replay PRIVATE pspocket::rsx_metal)
    target_compile_options(rsx-replay PRIVATE -fobjc-arc)
    set_target_properties(rsx-replay PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    pspocket_set_warnings(rsx-replay)
endif()
