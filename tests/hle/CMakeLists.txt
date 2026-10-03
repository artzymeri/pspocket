# HLE linkage and the process loader: registry checks, stub calls, and a synthetic executable booted end to end.
pspocket_add_test(test_hle
    SOURCES test_hle.cpp
    LIBS pspocket::loader pspocket_test_synthetic pspocket::recomp_ppu)
