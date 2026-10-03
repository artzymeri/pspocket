# RSX tests. Inputs are synthetic command buffers, microcode and texture data built in code; never game data.

# Front end: FIFO, methods, state, shader microcode, texture layouts, write tracking. Runs anywhere.
pspocket_add_test(test_rsx
    SOURCES test_fifo.cpp test_state.cpp test_methods.cpp test_shader.cpp test_texture.cpp test_write_tracker.cpp
    LIBS pspocket::rsx)
target_include_directories(test_rsx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
