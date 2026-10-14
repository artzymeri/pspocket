/*
 * The native implementations (C11, with the helpers lifted code uses: pspocket/spu/lifted.h, so each SPU operation
 * gives what the lifted code's does). Each is a pspocket::spu::replace::Impl (pspocket/spu/replace.hpp): 1 when it
 * did the function's work, 0 when it declined having changed nothing.
 */
#ifndef PSPOCKET_SPU_REPLACE_NATIVE_H
#define PSPOCKET_SPU_REPLACE_NATIVE_H

#include <stdint.h>

#include "pspocket/spu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EDGE geometry, the build of spu_dad457863220b1cc (edge_dad4.c). */
int ps_edge_dad4_0ecf8(SPUContext* ctx, uint8_t* ls);
int ps_edge_dad4_0f008(SPUContext* ctx, uint8_t* ls);

/* Sony's MP3 decoder module (flashMP3.pic), program 884ae88f68a9e778: decodeFrame (flashmp3.c). */
int ps_flashmp3_884a_1e820(SPUContext* ctx, uint8_t* ls);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_SPU_REPLACE_NATIVE_H */
