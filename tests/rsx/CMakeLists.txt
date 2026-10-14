# RSX tests. Inputs are synthetic command buffers, microcode and texture data built in code; never game data.

# Front end: FIFO, methods, state, shader microcode, texture layouts, write tracking. Runs anywhere.
pspocket_add_test(test_rsx
    SOURCES test_fifo.cpp test_state.cpp test_methods.cpp test_shader.cpp test_texture.cpp test_write_tracker.cpp
        bench_fifo.cpp
    LIBS pspocket::rsx)
target_include_directories(test_rsx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Metal backend end to end (Objective-C++ with ARC; Apple platforms with a Metal device, else the cases skip).
if(TARGET pspocket::rsx_metal)
    enable_language(OBJCXX)
    pspocket_add_test(test_rsx_metal
        SOURCES test_metal.mm
        LIBS pspocket::rsx_metal)
    target_include_directories(test_rsx_metal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(test_rsx_metal PRIVATE -fobjc-arc)
    set_target_properties(test_rsx_metal PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    # Every end-to-end draw again through the shader interpreter (D-940): the same pixels as the translation's. The
    # cases that count compiles are left out (the interpreter's functions are compiles of their own), and the signed
    # blending the translation does in the shader (D-948: the interpreter's draws blend as Metal can).
    add_test(NAME test_rsx_metal_interpreted COMMAND test_rsx_metal "metal e2e:*" "~*(D-114)" "~*(D-158)" "~*(D-940)"
        "~*(D-948)")
    set_tests_properties(test_rsx_metal_interpreted PROPERTIES ENVIRONMENT PSPOCKET_RSX_INTERPRETER=1)
endif()
