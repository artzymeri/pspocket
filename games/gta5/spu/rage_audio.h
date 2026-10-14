/*
 * Native implementations of GTA V's RAGE audio job functions (D-979 to D-983): each a
 * pspocket::spu::replace::Impl (pspocket/spu/replace.hpp), 1 when it did the function's work, 0 when it declined
 * having changed nothing.
 */
#ifndef PSPOCKET_GAMES_GTA5_RAGE_AUDIO_H
#define PSPOCKET_GAMES_GTA5_RAGE_AUDIO_H

#include <stdint.h>

#include "pspocket/spu/context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The synthesizer's job, spu_012bf13511b9f1b0. */
int ps_gta5_012b_05b20(SPUContext* ctx, uint8_t* ls); /* two biquad sections in series */
int ps_gta5_012b_062c8(SPUContext* ctx, uint8_t* ls); /* biquad */

/* The mixer's submix job, spu_28281614459ce75e (rage_submix.c). */
int ps_gta5_2828_06048(SPUContext* ctx, uint8_t* ls); /* reverb (Freeverb's structure) */
int ps_gta5_2828_08748(SPUContext* ctx, uint8_t* ls); /* delay */
int ps_gta5_2828_0a920(SPUContext* ctx, uint8_t* ls); /* reverb */

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_GAMES_GTA5_RAGE_AUDIO_H */
