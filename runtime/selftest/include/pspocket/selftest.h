/*
 * Device self-test: probes that check the platform assumptions the runtime depends on (memory reservation,
 * fault handling, timers) and measure the building blocks (interpreter and lifted-code throughput).
 * C API so the Swift app shell and the macOS CLI can both drive it.
 */
#ifndef PSPOCKET_SELFTEST_H
#define PSPOCKET_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ps_selftest_event {
    PS_SELFTEST_BEGIN = 0, /* a probe starts; message is its one-line description */
    PS_SELFTEST_INFO = 1,  /* free-form detail */
    PS_SELFTEST_METRIC = 2, /* "name = value unit" measurement */
    PS_SELFTEST_PASS = 3,  /* probe finished and passed; message is a summary */
    PS_SELFTEST_FAIL = 4,  /* probe finished and failed; message says why */
    PS_SELFTEST_SKIP = 5,  /* probe not applicable on this platform/build */
} ps_selftest_event;

/* Called on the thread that runs the probes. `probe` is the probe's name, `message` a UTF-8 line. */
typedef void (*ps_selftest_callback)(void* user, ps_selftest_event event, const char* probe, const char* message);

typedef struct ps_selftest_summary {
    int passed;
    int failed;
    int skipped;
    double seconds;
} ps_selftest_summary;

int ps_selftest_probe_count(void);
const char* ps_selftest_probe_name(int index);
const char* ps_selftest_probe_description(int index);

/* Runs one probe / all probes on the calling thread (blocking). */
ps_selftest_summary ps_selftest_run_one(int index, ps_selftest_callback callback, void* user);
ps_selftest_summary ps_selftest_run_all(ps_selftest_callback callback, void* user);

/* Version, build type, compiler and target, for the report header. */
const char* ps_selftest_build_info(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_SELFTEST_H */
