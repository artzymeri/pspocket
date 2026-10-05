# Route B's compiler on the device (D-151): only with an LLVM build (PSPOCKET_LLVM_DIR).
if(TARGET pspocket_codegen)
    pspocket_add_test(test_codegen
        SOURCES test_codegen.cpp
        LIBS pspocket::codegen pspocket::codesign)
endif()
