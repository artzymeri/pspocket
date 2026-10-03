# runtime/spu — SPU interpreter, local store, MFC, channels, reservations (Phase 3)

Not started. Plan section 6: 256 KB local store per SPU thread, synchronous DMA, channels as blocking host waits,
and the one reservation table shared with the PPU (`lwarx`/`stwcx.` and GETLLAR/PUTLLC on 128-byte lines).
`replace/` holds native replacements for known SPU programs, keyed by hash, each with an A/B test.
