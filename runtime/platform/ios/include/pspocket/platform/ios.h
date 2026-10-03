/*
 * iOS host glue: routes the runtime's log to os_log and the app, and reports what jetsam sees.
 * C API so the Swift app shell can call it through its bridging header.
 */
#ifndef PSPOCKET_PLATFORM_IOS_H
#define PSPOCKET_PLATFORM_IOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same order and values as pspocket::log::Level. */
typedef enum ps_ios_log_level {
    PS_IOS_LOG_TRACE = 0,
    PS_IOS_LOG_DEBUG = 1,
    PS_IOS_LOG_INFO = 2,
    PS_IOS_LOG_WARN = 3,
    PS_IOS_LOG_ERROR = 4,
    PS_IOS_LOG_FATAL = 5,
} ps_ios_log_level;

/* Called on the logging thread with every line that passes the runtime's minimum level. */
typedef void (*ps_ios_log_callback)(void* user, ps_ios_log_level level, const char* channel, const char* message);

/*
 * Installs the runtime log sink: every line goes to os_log (subsystem "com.artzymeri.pspocket", category = channel)
 * and then to `callback`, or to stderr when `callback` is NULL. Call again to replace the callback.
 */
void ps_ios_install_log_sink(ps_ios_log_callback callback, void* user);

/* Process memory footprint in bytes (task_vm_info phys_footprint, the number jetsam enforces). 0 on failure. */
uint64_t ps_ios_memory_footprint(void);

/* Bytes this process can still allocate before jetsam terminates it (os_proc_available_memory). 0 if unknown. */
uint64_t ps_ios_available_memory(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_PLATFORM_IOS_H */
