pspocket_add_test(test_core
    SOURCES test_audio_ring.cpp test_be.cpp test_guest_memory.cpp test_png.cpp test_timebase.cpp
    LIBS pspocket::core)

# Reservation litmus/stress tests (time-bounded; PSPOCKET_STRESS_SECONDS sets the duration per test).
pspocket_add_test(test_reservation
    SOURCES test_reservation.cpp
    LIBS pspocket::core)
