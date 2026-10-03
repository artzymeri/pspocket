# runtime/rsx — RSX command stream to Metal (Phase 4)

Not started. FIFO parser (NV4097 + JUMP/CALL/RETURN, semaphores, labels, reports), state tracking, surface and
texture caches with write tracking (the `mem.fault_tracking` self-test probe measures the fault path), vertex
pulling, shader translation to MSL with an `MTLBinaryArchive` cache. `metal/` holds the backend.
