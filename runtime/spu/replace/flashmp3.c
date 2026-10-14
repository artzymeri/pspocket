/*
 * Sony's MP3 decoder module (dev_flash/sys/external/flashMP3.pic), natively (D-974).
 *
 * Sony's MultiStream audio library loads the module into its SPU decode jobs (GTA V's mixer: "Mp3SpuDecode") and calls
 * decodeFrame once a frame; every game that plays MP3 through MultiStream loads the same build (program
 * 884ae88f68a9e778, the image from file offset 0x100, lifted at 0x17400). The module is Michael Hipp's mpglib layer III
 * decoder (its symbols: layer3.c, decoder.c, III_get_side_info, III_dehuffman, III_hybrid, synth_filterbank...) built
 * for the SPU, its state kept by the caller: decodeFrame(frame, &consumed, &pcm_bytes, heap, oldBlock[2], buffs[2],
 * backup, &bufferLoc) with
 *
 *   heap       22528 bytes of scratch the module zeroes and allocates from: the bitstream buffer (2304: 512 bytes of
 *              the frames before, then this frame), the PCM (2 x 4096 bytes: 16-bit samples, channel 0 then channel 1
 *              at +4096, planar), the granule's spectra and the hybrid filter's output;
 *   oldBlock   576 floats a channel: mpglib's hybrid_block, the overlap of the previous granule's IMDCT;
 *   buffs      2 x 272 floats a channel: mpglib's synth_buffs, the polyphase filterbank's history;
 *   backup     the last 512 bytes of the frame before (the bit reservoir);
 *   bufferLoc  the filterbank's position (mpglib's synth_bo).
 *
 * It returns the PCM's address (heap + 2304) and the bytes of PCM a channel in pcm_bytes (-1 for a Xing/Info frame),
 * the bytes the frame took in consumed; an ID3v2 tag or an ID3v1 "TAG" is skipped (0 and no PCM), a broken header
 * consumes nothing and returns -1 after printing to the PPU.
 *
 * This file decodes as the module does, from the same algorithm (ISO/IEC 11172-3 and 13818-3 layer III through
 * mpglib's structure: LAME 3.100's mpglib, LGPL, is the reference for every step), on the host: the state the module
 * keeps goes in and out of local store in the module's own layout, so native and lifted calls interleave freely, and
 * whatever the module leaves in its own variables and the heap is left as it leaves it. The PCM and the float state are
 * equivalent rather than bit-exact (the host's float operations in another order than the SPU's): the native code runs
 * in the SPU FP environment (round toward zero, denormals flushed), as the module's float code does. It declines
 * (changes nothing, the lifted module runs) what it does not handle: the paths that print to the PPU (a broken header,
 * intensity stereo, which the module refuses), reserved header fields and malformed side information.
 *
 * Layout facts (the module's symbol table): the image's vaddr v is at module + v - 0x80; decodeFrame is v 0x74A0; its
 * variables are below (VAR_*), each in a quadword of its own.
 */
#include "native.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "mp3_huffman.h"
#include "pspocket/core/be.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/runtime.h"

#define LS_MASK PS_SPU_LS_MASK

/* ---- the module ---- */

enum {
    MOD_LIFTED_AT = 0x17400, /* the program's code start */
    MOD_V0 = 0x80,           /* the image's first vaddr */
    /* .data/.bss: vaddr - 0x80 */
    VAR_globalSpaceAvailable = 0x10000 - MOD_V0,
    VAR_pcm_point = 0x10010 - MOD_V0,
    VAR_xing_header = 0x10020 - MOD_V0,
    VAR_oldBlockCh1 = 0x10040 - MOD_V0,
    VAR_rval = 0x100A0 - MOD_V0,
    VAR_buffs0 = 0x10200 - MOD_V0,
    VAR_rval_uc = 0x10210 - MOD_V0,
    VAR_oldBlockCh0 = 0x10280 - MOD_V0,
    VAR_buffs1 = 0x10290 - MOD_V0,
    VAR_startTimer = 0x102B0 - MOD_V0,
    VAR_bufferLoc = 0x102C0 - MOD_V0,
    VAR_pcm_sample = 0x102D0 - MOD_V0,
    VAR_globalHeap = 0x102E0 - MOD_V0,
    VAR_bsbuf = 0x102F0 - MOD_V0,
    VAR_procTime = 0x10320 - MOD_V0,
    VAR_bsspace = 0x10330 - MOD_V0,
    VAR_fr = 0x10340 - MOD_V0,
    VAR_globalSpaceTaken = 0x10390 - MOD_V0,
    VAR_ssize = 0x103A0 - MOD_V0,
    VAR_bsi = 0x103B0 - MOD_V0,
    VAR_endTimer = 0x103C0 - MOD_V0,
    VAR_backup = 0x103D0 - MOD_V0,
};

/* The heap as decodeFrame and decodePacket allocate it (128-byte steps). */
enum {
    HEAP_SIZE = 22528,
    HEAP_BSSPACE = 0,         /* 2304 */
    HEAP_PCM = 2304,          /* 8196: channel 0, channel 1 at +4096 */
    HEAP_HYBRID_IN = 10624,   /* 2 x 576 floats */
    HEAP_HYBRID_OUT = 15232,  /* 2 x 576 floats */
    HEAP_HYBRID_TEMP = 19840, /* III_hybrid's new block, 576 floats, freed after each call */
    HEAP_TAKEN_AFTER = 19840, /* globalSpaceTaken after a frame */
    BS_SIZE = 2304,
    PCM_CH1 = 4096,
};

#define SBLIMIT 32
#define SSLIMIT 18

/* ---- tables (computed once, as mpglib's init: tabinit.c make_decode_tables, layer3.c init_layer3) ---- */

static float g_gainpow2[256 + 118 + 4];
static float g_ispow[8207];
static float g_aa_cs[8], g_aa_ca[8];
static float g_win[4][36], g_win1[4][36];
static float g_COS9[9], g_tfcos36[9], g_tfcos12[3], g_COS6_1, g_COS6_2;
static float g_decwin[512 + 32];
static float g_cos64[16], g_cos32[8], g_cos16[4], g_cos8[2], g_cos4[1];
static int g_longLimit[9][23], g_shortLimit[9][14];
static int g_mapbuf0[9][152], g_mapbuf1[9][156], g_mapbuf2[9][44];
static int *g_map[9][3], *g_mapend[9][3];
static unsigned g_n_slen2[512], g_i_slen2[256];

typedef struct {
    short longIdx[23], longDiff[22], shortIdx[14], shortDiff[13];
} band_info;

/* clang-format off */
static const band_info g_bandInfo[9] = {
    /* MPEG 1.0 */
    {{0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418,576},
     {4,4,4,4,4,4,6,6,8,8,10,12,16,20,24,28,34,42,50,54,76,158},
     {0,4*3,8*3,12*3,16*3,22*3,30*3,40*3,52*3,66*3,84*3,106*3,136*3,192*3},
     {4,4,4,4,6,8,10,12,14,18,22,30,56}},
    {{0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384,576},
     {4,4,4,4,4,4,6,6,6,8,10,12,16,18,22,28,34,40,46,54,54,192},
     {0,4*3,8*3,12*3,16*3,22*3,28*3,38*3,50*3,64*3,80*3,100*3,126*3,192*3},
     {4,4,4,4,6,6,10,12,14,16,20,26,66}},
    {{0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550,576},
     {4,4,4,4,4,4,6,6,8,10,12,16,20,24,30,38,46,56,68,84,102,26},
     {0,4*3,8*3,12*3,16*3,22*3,30*3,42*3,58*3,78*3,104*3,138*3,180*3,192*3},
     {4,4,4,4,6,8,12,16,20,26,34,42,12}},
    /* MPEG 2.0 */
    {{0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
     {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
     {0,4*3,8*3,12*3,18*3,24*3,32*3,42*3,56*3,74*3,100*3,132*3,174*3,192*3},
     {4,4,4,6,6,8,10,14,18,26,32,42,18}},
    {{0,6,12,18,24,30,36,44,54,66,80,96,114,136,162,194,232,278,332,394,464,540,576},
     {6,6,6,6,6,6,8,10,12,14,16,18,22,26,32,38,46,54,62,70,76,36},
     {0,4*3,8*3,12*3,18*3,26*3,36*3,48*3,62*3,80*3,104*3,136*3,180*3,192*3},
     {4,4,4,6,8,10,12,14,18,24,32,44,12}},
    {{0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
     {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
     {0,4*3,8*3,12*3,18*3,26*3,36*3,48*3,62*3,80*3,104*3,134*3,174*3,192*3},
     {4,4,4,6,8,10,12,14,18,24,30,40,18}},
    /* MPEG 2.5 */
    {{0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
     {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
     {0,12,24,36,54,78,108,144,186,240,312,402,522,576},
     {4,4,4,6,8,10,12,14,18,24,30,40,18}},
    {{0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
     {6,6,6,6,6,6,8,10,12,14,16,20,24,28,32,38,46,52,60,68,58,54},
     {0,12,24,36,54,78,108,144,186,240,312,402,522,576},
     {4,4,4,6,8,10,12,14,18,24,30,40,18}},
    {{0,12,24,36,48,60,72,88,108,132,160,192,232,280,336,400,476,566,568,570,572,574,576},
     {12,12,12,12,12,12,16,20,24,28,32,40,48,56,64,76,90,2,2,2,2,2},
     {0,24,48,72,108,156,216,288,372,480,486,492,498,576},
     {8,8,8,12,16,20,24,28,36,2,2,2,26}},
};

/* ISO/IEC 11172-3 table 3-B.8's window (mpglib tabinit.c dewin), D[i] for i = 0..256. */
static const double g_dewin[257] = {
     0.000000000,-0.000015259,-0.000015259,-0.000015259,-0.000015259,-0.000015259,-0.000015259,-0.000030518,
    -0.000030518,-0.000030518,-0.000030518,-0.000045776,-0.000045776,-0.000061035,-0.000061035,-0.000076294,
    -0.000076294,-0.000091553,-0.000106812,-0.000106812,-0.000122070,-0.000137329,-0.000152588,-0.000167847,
    -0.000198364,-0.000213623,-0.000244141,-0.000259399,-0.000289917,-0.000320435,-0.000366211,-0.000396729,
    -0.000442505,-0.000473022,-0.000534058,-0.000579834,-0.000625610,-0.000686646,-0.000747681,-0.000808716,
    -0.000885010,-0.000961304,-0.001037598,-0.001113892,-0.001205444,-0.001296997,-0.001388550,-0.001480103,
    -0.001586914,-0.001693726,-0.001785278,-0.001907349,-0.002014160,-0.002120972,-0.002243042,-0.002349854,
    -0.002456665,-0.002578735,-0.002685547,-0.002792358,-0.002899170,-0.002990723,-0.003082275,-0.003173828,
    -0.003250122,-0.003326416,-0.003387451,-0.003433228,-0.003463745,-0.003479004,-0.003479004,-0.003463745,
    -0.003417969,-0.003372192,-0.003280640,-0.003173828,-0.003051758,-0.002883911,-0.002700806,-0.002487183,
    -0.002227783,-0.001937866,-0.001617432,-0.001266479,-0.000869751,-0.000442505, 0.000030518, 0.000549316,
     0.001098633, 0.001693726, 0.002334595, 0.003005981, 0.003723145, 0.004486084, 0.005294800, 0.006118774,
     0.007003784, 0.007919312, 0.008865356, 0.009841919, 0.010848999, 0.011886597, 0.012939453, 0.014022827,
     0.015121460, 0.016235352, 0.017349243, 0.018463135, 0.019577026, 0.020690918, 0.021789551, 0.022857666,
     0.023910522, 0.024932861, 0.025909424, 0.026840210, 0.027725220, 0.028533936, 0.029281616, 0.029937744,
     0.030532837, 0.031005859, 0.031387329, 0.031661987, 0.031814575, 0.031845093, 0.031738281, 0.031478882,
     0.031082153, 0.030517578, 0.029785156, 0.028884888, 0.027801514, 0.026535034, 0.025085449, 0.023422241,
     0.021575928, 0.019531250, 0.017257690, 0.014801025, 0.012115479, 0.009231567, 0.006134033, 0.002822876,
    -0.000686646,-0.004394531,-0.008316040,-0.012420654,-0.016708374,-0.021179199,-0.025817871,-0.030609131,
    -0.035552979,-0.040634155,-0.045837402,-0.051132202,-0.056533813,-0.061996460,-0.067520142,-0.073059082,
    -0.078628540,-0.084182739,-0.089706421,-0.095169067,-0.100540161,-0.105819702,-0.110946655,-0.115921021,
    -0.120697021,-0.125259399,-0.129562378,-0.133590698,-0.137298584,-0.140670776,-0.143676758,-0.146255493,
    -0.148422241,-0.150115967,-0.151306152,-0.151962280,-0.152069092,-0.151596069,-0.150497437,-0.148773193,
    -0.146362305,-0.143264771,-0.139450073,-0.134887695,-0.129577637,-0.123474121,-0.116577148,-0.108856201,
    -0.100311279,-0.090927124,-0.080688477,-0.069595337,-0.057617187,-0.044784546,-0.031082153,-0.016510010,
    -0.001068115, 0.015228271, 0.032379150, 0.050354004, 0.069168091, 0.088775635, 0.109161377, 0.130310059,
     0.152206421, 0.174789429, 0.198059082, 0.221984863, 0.246505737, 0.271591187, 0.297210693, 0.323318481,
     0.349868774, 0.376800537, 0.404083252, 0.431655884, 0.459472656, 0.487472534, 0.515609741, 0.543823242,
     0.572036743, 0.600219727, 0.628295898, 0.656219482, 0.683914185, 0.711318970, 0.738372803, 0.765029907,
     0.791213989, 0.816864014, 0.841949463, 0.866363525, 0.890090942, 0.913055420, 0.935195923, 0.956481934,
     0.976852417, 0.996246338, 1.014617920, 1.031936646, 1.048156738, 1.063217163, 1.077117920, 1.089782715,
     1.101211548, 1.111373901, 1.120223999, 1.127746582, 1.133926392, 1.138763428, 1.142211914, 1.144287109,
     1.144989014,
};

/* Layer III bit rates (kbit/s) by lsf and index, and the sampling frequencies by the module's index. */
static const int g_tabsel3[2][16] = {
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
};
static const int g_freqs[9] = {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000};
static const int g_pretab1[22] = {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0};
static const int g_pretab2[22] = {0};
static const unsigned char g_slen[2][16] = {
    {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4},
    {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3},
};
static const unsigned char g_stab[3][6][4] = {
    {{6, 5, 5, 5}, {6, 5, 7, 3}, {11, 10, 0, 0}, {7, 7, 7, 0}, {6, 6, 6, 3}, {8, 8, 5, 0}},
    {{9, 9, 9, 9}, {9, 9, 12, 6}, {18, 18, 0, 0}, {12, 12, 12, 0}, {12, 9, 9, 6}, {15, 12, 9, 0}},
    {{6, 9, 9, 9}, {6, 9, 12, 6}, {15, 18, 0, 0}, {6, 15, 12, 0}, {6, 12, 9, 6}, {6, 18, 9, 0}},
};
/* clang-format on */

static void init_tables(void)
{
    /* The tables in double precision and the host's default rounding, whatever the caller's environment. */
    int i, j, k;
    for (i = -256; i < 118 + 4; i++) g_gainpow2[i + 256] = (float)pow(2.0, -0.25 * (double)(i + 210));
    for (i = 0; i < 8207; i++) g_ispow[i] = (float)pow((double)i, 4.0 / 3.0);
    for (i = 0; i < 8; i++) {
        static const double Ci[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        const double sq = sqrt(1.0 + Ci[i] * Ci[i]);
        g_aa_cs[i] = (float)(1.0 / sq);
        g_aa_ca[i] = (float)(Ci[i] / sq);
    }
    for (i = 0; i < 18; i++) {
        g_win[0][i] = g_win[1][i] = (float)(0.5 * sin(M_PI / 72.0 * (double)(2 * i + 1)) / cos(M_PI * (double)(2 * i + 19) / 72.0));
        g_win[0][i + 18] = g_win[3][i + 18] =
            (float)(0.5 * sin(M_PI / 72.0 * (double)(2 * (i + 18) + 1)) / cos(M_PI * (double)(2 * (i + 18) + 19) / 72.0));
    }
    for (i = 0; i < 6; i++) {
        g_win[1][i + 18] = (float)(0.5 / cos(M_PI * (double)(2 * (i + 18) + 19) / 72.0));
        g_win[3][i + 12] = (float)(0.5 / cos(M_PI * (double)(2 * (i + 12) + 19) / 72.0));
        g_win[1][i + 24] = (float)(0.5 * sin(M_PI / 24.0 * (double)(2 * i + 13)) / cos(M_PI * (double)(2 * (i + 24) + 19) / 72.0));
        g_win[1][i + 30] = g_win[3][i] = 0.0f;
        g_win[3][i + 6] = (float)(0.5 * sin(M_PI / 24.0 * (double)(2 * i + 1)) / cos(M_PI * (double)(2 * (i + 6) + 19) / 72.0));
    }
    for (i = 0; i < 9; i++) g_COS9[i] = (float)cos(M_PI / 18.0 * (double)i);
    for (i = 0; i < 9; i++) g_tfcos36[i] = (float)(0.5 / cos(M_PI * (double)(i * 2 + 1) / 36.0));
    for (i = 0; i < 3; i++) g_tfcos12[i] = (float)(0.5 / cos(M_PI * (double)(i * 2 + 1) / 12.0));
    g_COS6_1 = (float)cos(M_PI / 6.0 * 1.0);
    g_COS6_2 = (float)cos(M_PI / 6.0 * 2.0);
    for (i = 0; i < 12; i++)
        g_win[2][i] = (float)(0.5 * sin(M_PI / 24.0 * (double)(2 * i + 1)) / cos(M_PI * (double)(2 * i + 7) / 24.0));
    for (j = 0; j < 4; j++) {
        static const int len[4] = {36, 36, 12, 36};
        for (i = 0; i < len[j]; i += 2) g_win1[j][i] = +g_win[j][i];
        for (i = 1; i < len[j]; i += 2) g_win1[j][i] = -g_win[j][i];
    }

    for (j = 0; j < 9; j++) {
        const band_info* bi = &g_bandInfo[j];
        int *mp, cb, lwin;
        const short* bdf;
        const int switch_idx = (j < 3) ? 8 : 6;
        mp = g_map[j][0] = g_mapbuf0[j];
        bdf = bi->longDiff;
        for (i = 0, cb = 0; cb < switch_idx; cb++, i += *bdf++) {
            *mp++ = (*bdf) >> 1;
            *mp++ = i;
            *mp++ = 3;
            *mp++ = cb;
        }
        bdf = bi->shortDiff + 3;
        for (cb = 3; cb < 13; cb++) {
            const int l = (*bdf++) >> 1;
            for (lwin = 0; lwin < 3; lwin++) {
                *mp++ = l;
                *mp++ = i + lwin;
                *mp++ = lwin;
                *mp++ = cb;
            }
            i += 6 * l;
        }
        g_mapend[j][0] = mp;
        mp = g_map[j][1] = g_mapbuf1[j];
        bdf = bi->shortDiff;
        for (i = 0, cb = 0; cb < 13; cb++) {
            const int l = (*bdf++) >> 1;
            for (lwin = 0; lwin < 3; lwin++) {
                *mp++ = l;
                *mp++ = i + lwin;
                *mp++ = lwin;
                *mp++ = cb;
            }
            i += 6 * l;
        }
        g_mapend[j][1] = mp;
        mp = g_map[j][2] = g_mapbuf2[j];
        bdf = bi->longDiff;
        for (cb = 0; cb < 22; cb++) {
            *mp++ = (*bdf++) >> 1;
            *mp++ = cb;
        }
        g_mapend[j][2] = mp;
    }
    for (j = 0; j < 9; j++) {
        for (i = 0; i < 23; i++) {
            g_longLimit[j][i] = (g_bandInfo[j].longIdx[i] - 1 + 8) / 18 + 1;
            if (g_longLimit[j][i] > SBLIMIT) g_longLimit[j][i] = SBLIMIT;
        }
        for (i = 0; i < 14; i++) {
            g_shortLimit[j][i] = (g_bandInfo[j].shortIdx[i] - 1) / 18 + 1;
            if (g_shortLimit[j][i] > SBLIMIT) g_shortLimit[j][i] = SBLIMIT;
        }
    }
    for (i = 0; i < 5; i++)
        for (j = 0; j < 6; j++)
            for (k = 0; k < 6; k++) g_i_slen2[k + j * 6 + i * 36] = (unsigned)(i | (j << 3) | (k << 6) | (3 << 12));
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            for (k = 0; k < 4; k++) g_i_slen2[k + j * 4 + i * 16 + 180] = (unsigned)(i | (j << 3) | (k << 6) | (4 << 12));
    for (i = 0; i < 4; i++)
        for (j = 0; j < 3; j++) {
            g_i_slen2[j + i * 3 + 244] = (unsigned)(i | (j << 3) | (5 << 12));
            g_n_slen2[j + i * 3 + 500] = (unsigned)(i | (j << 3) | (2 << 12) | (1 << 15));
        }
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            for (k = 0; k < 4; k++)
                for (int l = 0; l < 4; l++)
                    g_n_slen2[l + k * 4 + j * 16 + i * 80] = (unsigned)(i | (j << 3) | (k << 6) | (l << 9) | (0 << 12));
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++)
            for (k = 0; k < 4; k++) g_n_slen2[k + j * 4 + i * 20 + 400] = (unsigned)(i | (j << 3) | (k << 6) | (1 << 12));

    /* The polyphase filterbank (tabinit.c make_decode_tables, scale 32768: 16-bit output). */
    {
        float* pnts[5] = {g_cos64, g_cos32, g_cos16, g_cos8, g_cos4};
        for (i = 0; i < 5; i++) {
            const int kr = 0x10 >> i, divv = 0x40 >> i;
            for (k = 0; k < kr; k++) pnts[i][k] = (float)(1.0 / (2.0 * cos(M_PI * ((double)k * 2.0 + 1.0) / (double)divv)));
        }
        float* table = g_decwin;
        long scaleval = -32768;
        for (i = 0, j = 0; i < 256; i++, j++, table += 32) {
            if (table < g_decwin + 512 + 16) table[16] = table[0] = (float)(g_dewin[j] * (double)scaleval);
            if (i % 32 == 31) table -= 1023;
            if (i % 64 == 63) scaleval = -scaleval;
        }
        for (; i < 512; i++, j--, table += 32) {
            if (table < g_decwin + 512 + 16) table[16] = table[0] = (float)(g_dewin[j] * (double)scaleval);
            if (i % 32 == 31) table -= 1023;
            if (i % 64 == 63) scaleval = -scaleval;
        }
    }
}

static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;

static void init_tables_default_fp(void)
{
#if defined(__aarch64__)
    /* The caller may be in the SPU FP environment (round toward zero): libm wants the default. */
    const uint64_t fpcr = ps_spu_fpcr_read();
    ps_spu_fpcr_write(fpcr & ~(PS_SPU_FPCR_RMODE | PS_SPU_FPCR_FZ));
    init_tables();
    ps_spu_fpcr_write(fpcr);
#else
    init_tables();
#endif
}

/* ---- the decoder's state for one call (host) ---- */

typedef struct {
    int stereo, lsf, mpeg25, lay, error_protection, bitrate_index, sampling_frequency, padding, extension, mode,
        mode_ext, copyright, original, emphasis, framesize;
} mp3_frame;

typedef struct {
    int scfsi;
    unsigned part2_3_length, big_values, scalefac_compress, block_type, mixed_block_flag, table_select[3];
    int pow2gain, full_gain[3]; /* indices into g_gainpow2 */
    unsigned maxband[3], maxbandl, maxb, region1start, region2start, preflag, scalefac_scale, count1table_select;
} gr_info;

typedef struct {
    unsigned main_data_begin, private_bits;
    struct {
        gr_info gr[2];
    } ch[2];
} side_info;

typedef struct {
    /* the bitstream buffer, as the module's (bsspace: 512 bytes of reservoir, then the frame), and room past it for
     * reads that run off its end (a decline) */
    uint8_t bs[BS_SIZE + 8192];
    uint32_t wp, bi; /* mpglib's wordpointer (into bs) and bitindex */
    uint32_t rval;   /* the module's rval and rval_uc after its last read */
    uint8_t rval_uc;
    int bad; /* something the native code does not do as the module does: decline */
    mp3_frame fr;
    side_info si;
    int ssize;
    int scalefacs[2][39];
    float hybridIn[2][SBLIMIT][SSLIMIT];
    float hybridOut[2][SSLIMIT][SBLIMIT];
    float block[2][SBLIMIT * SSLIMIT];      /* oldBlock[ch] */
    float newblock[SBLIMIT * SSLIMIT];      /* III_hybrid's temporary block (the last one stays in the heap) */
    float buffs[2][2][0x110];                /* buffs[ch] */
    int bo;                                  /* bufferLoc */
    int16_t pcm[2][2048];                    /* channel 0, channel 1 */
    int pcm_point;                           /* bytes a channel */
} mp3_state;

static _Thread_local mp3_state t_state;

/* ---- bits (mpglib common.c getbits, getbits_fast, get1bit; the module keeps the last value in rval/rval_uc) ---- */

static inline uint32_t getbits(mp3_state* d, int n)
{
    const uint8_t* p = d->bs + d->wp;
    uint32_t rval = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    rval = (rval << d->bi) & 0xFFFFFFu;
    d->bi += (uint32_t)n;
    rval >>= 24 - n;
    d->wp += d->bi >> 3;
    d->bi &= 7;
    d->rval = rval;
    return rval;
}

static inline uint32_t get1bit(mp3_state* d)
{
    const uint8_t v = (uint8_t)(d->bs[d->wp] << d->bi);
    d->bi++;
    d->wp += d->bi >> 3;
    d->bi &= 7;
    d->rval_uc = v;
    return v >> 7;
}

/* ---- side information (the module's III_get_side_info: mpglib's III_get_side_info_1 and _2) ---- */

static void get_side_info(mp3_state* d, int stereo, int ms_stereo, int sfreq)
{
    side_info* si = &d->si;
    const int lsf = d->fr.lsf;
    si->main_data_begin = getbits(d, lsf ? 8 : 9);
    if (stereo == 1) si->private_bits = lsf ? get1bit(d) : getbits(d, 5);
    else si->private_bits = getbits(d, lsf ? 2 : 3);
    if (!lsf)
        for (int ch = 0; ch < stereo; ch++) {
            si->ch[ch].gr[0].scfsi = -1;
            si->ch[ch].gr[1].scfsi = (int)getbits(d, 4);
        }
    for (int gr = 0; gr < (lsf ? 1 : 2); gr++)
        for (int ch = 0; ch < stereo; ch++) {
            gr_info* g = &si->ch[ch].gr[gr];
            g->part2_3_length = getbits(d, 12);
            g->big_values = getbits(d, 9);
            if (g->big_values > 288) d->bad = 1; /* past the spectrum: the module writes past its arrays */
            g->pow2gain = 256 - (int)getbits(d, 8);
            if (ms_stereo) g->pow2gain += 2;
            g->scalefac_compress = getbits(d, lsf ? 9 : 4);
            if (get1bit(d)) {
                g->block_type = getbits(d, 2);
                g->mixed_block_flag = get1bit(d);
                g->table_select[0] = getbits(d, 5);
                g->table_select[1] = getbits(d, 5);
                g->table_select[2] = 0;
                for (int i = 0; i < 3; i++) g->full_gain[i] = g->pow2gain + (int)(getbits(d, 3) << 3);
                if (g->block_type == 0) d->bad = 1; /* forbidden */
                if (!lsf) {
                    g->region1start = 36 >> 1;
                } else if (g->block_type == 2) {
                    g->region1start = g->mixed_block_flag ? 48 >> 1 : 36 >> 1;
                } else {
                    g->region1start = 54 >> 1;
                }
                if (lsf && sfreq == 8) g->region1start *= 2;
                g->region2start = 576 >> 1;
            } else {
                for (int i = 0; i < 3; i++) g->table_select[i] = getbits(d, 5);
                const unsigned r0c = getbits(d, 4), r1c = getbits(d, 3);
                if (r0c + 1 + r1c + 1 > 22) d->bad = 1; /* past bandInfo's long bands */
                g->region1start = (unsigned)g_bandInfo[sfreq].longIdx[(r0c + 1) > 22 ? 22 : r0c + 1] >> 1;
                g->region2start = (unsigned)g_bandInfo[sfreq].longIdx[(r0c + r1c + 2) > 22 ? 22 : r0c + r1c + 2] >> 1;
                g->block_type = 0;
                g->mixed_block_flag = 0;
            }
            if (!lsf) g->preflag = get1bit(d);
            g->scalefac_scale = get1bit(d);
            g->count1table_select = get1bit(d);
        }
}

/* ---- scale factors (mpglib III_get_scale_factors_1 and _2) ---- */

static int get_scale_factors_1(mp3_state* d, int* scf, const gr_info* g)
{
    const int num0 = g_slen[0][g->scalefac_compress], num1 = g_slen[1][g->scalefac_compress];
    int numbits;
    if (g->block_type == 2) {
        int i = 18;
        numbits = (num0 + num1) * 18;
        if (g->mixed_block_flag) {
            for (i = 8; i; i--) *scf++ = (int)getbits(d, num0);
            i = 9;
            numbits -= num0;
        }
        for (; i; i--) *scf++ = (int)getbits(d, num0);
        for (i = 18; i; i--) *scf++ = (int)getbits(d, num1);
        *scf++ = 0;
        *scf++ = 0;
        *scf++ = 0;
    } else {
        const int scfsi = g->scfsi;
        if (scfsi < 0) {
            for (int i = 11; i; i--) *scf++ = (int)getbits(d, num0);
            for (int i = 10; i; i--) *scf++ = (int)getbits(d, num1);
            numbits = (num0 + num1) * 10 + num0;
        } else {
            numbits = 0;
            if (!(scfsi & 0x8)) {
                for (int i = 6; i; i--) *scf++ = (int)getbits(d, num0);
                numbits += num0 * 6;
            } else {
                scf += 6;
            }
            if (!(scfsi & 0x4)) {
                for (int i = 5; i; i--) *scf++ = (int)getbits(d, num0);
                numbits += num0 * 5;
            } else {
                scf += 5;
            }
            if (!(scfsi & 0x2)) {
                for (int i = 5; i; i--) *scf++ = (int)getbits(d, num1);
                numbits += num1 * 5;
            } else {
                scf += 5;
            }
            if (!(scfsi & 0x1)) {
                for (int i = 5; i; i--) *scf++ = (int)getbits(d, num1);
                numbits += num1 * 5;
            } else {
                scf += 5;
            }
        }
        *scf++ = 0;
    }
    return numbits;
}

static int get_scale_factors_2(mp3_state* d, int* scf, gr_info* g, int i_stereo)
{
    unsigned slen = i_stereo ? g_i_slen2[g->scalefac_compress >> 1] : g_n_slen2[g->scalefac_compress];
    int numbits = 0, n = 0;
    g->preflag = (slen >> 15) & 0x1;
    if (g->block_type == 2) {
        n++;
        if (g->mixed_block_flag) n++;
    }
    const unsigned char* pnt = g_stab[n][(slen >> 12) & 0x7];
    for (int i = 0; i < 4; i++) {
        const int num = (int)(slen & 0x7);
        slen >>= 3;
        if (num) {
            for (int j = 0; j < (int)pnt[i]; j++) *scf++ = (int)getbits(d, num);
            numbits += pnt[i] * num;
        } else {
            for (int j = 0; j < (int)pnt[i]; j++) *scf++ = 0;
        }
    }
    n = (n << 1) + 1;
    for (int i = 0; i < n; i++) *scf++ = 0;
    return numbits;
}

/* ---- Huffman decoding and dequantisation (mpglib III_dequantize_sample) ---- */

static inline int huff_pair(mp3_state* d, const int16_t* val, int* part2remain)
{
    int y;
    while ((y = *val++) < 0) {
        if (get1bit(d)) val -= y;
        (*part2remain)--;
    }
    return y;
}

static inline float lin_value(mp3_state* d, int x, unsigned linbits, float v, int* part2remain)
{
    if (x == 15) {
        *part2remain -= (int)linbits + 1;
        x += (int)getbits(d, (int)linbits);
        return get1bit(d) ? -g_ispow[x] * v : g_ispow[x] * v;
    }
    if (x) {
        const float r = get1bit(d) ? -g_ispow[x] * v : g_ispow[x] * v;
        (*part2remain)--;
        return r;
    }
    return 0.0f;
}

static void dequantize_sample(mp3_state* d, float xr[SBLIMIT][SSLIMIT], int* scf, gr_info* g, int sfreq, int part2bits)
{
    const int shift = 1 + (int)g->scalefac_scale;
    float* xrpnt = (float*)xr;
    int l[3], l3;
    int part2remain = (int)g->part2_3_length - part2bits;
    float* const xr_end = &xr[SBLIMIT - 1][SSLIMIT - 1];

    memset(xr, 0, sizeof(float) * SBLIMIT * SSLIMIT);
    {
        const int bv = (int)g->big_values, region1 = (int)g->region1start, region2 = (int)g->region2start;
        l3 = ((576 >> 1) - bv) >> 1;
        if (bv <= region1) {
            l[0] = bv;
            l[1] = 0;
            l[2] = 0;
        } else {
            l[0] = region1;
            if (bv <= region2) {
                l[1] = bv - l[0];
                l[2] = 0;
            } else {
                l[1] = region2 - l[0];
                l[2] = bv - region2;
            }
        }
        for (int i = 0; i < 3; i++)
            if (l[i] < 0) d->bad = 1;
    }
#define PUT(v)                                                                                                         \
    do {                                                                                                               \
        if (xrpnt <= xr_end) *xrpnt = (v);                                                                             \
        else d->bad = 1;                                                                                               \
    } while (0)

    if (g->block_type == 2) {
        int max[4], step = 0, lwin = 0, cb = 0, mc = 0;
        float v = 0.0f;
        int *m, *me;
        if (g->mixed_block_flag) {
            max[3] = -1;
            max[0] = max[1] = max[2] = 2;
            m = g_map[sfreq][0];
            me = g_mapend[sfreq][0];
        } else {
            max[0] = max[1] = max[2] = max[3] = -1;
            m = g_map[sfreq][1];
            me = g_mapend[sfreq][1];
        }
        for (int i = 0; i < 2; i++) {
            int lp = l[i];
            const mp3_huffman_table* h = &mp3_ht[g->table_select[i]];
            for (; lp; lp--, mc--) {
                if (!mc) {
                    if (m >= me) {
                        d->bad = 1;
                        return;
                    }
                    mc = *m++;
                    xrpnt = ((float*)xr) + (*m++);
                    lwin = *m++;
                    cb = *m++;
                    if (lwin == 3) {
                        v = g_gainpow2[g->pow2gain + ((*scf++) << shift)];
                        step = 1;
                    } else {
                        v = g_gainpow2[g->full_gain[lwin] + ((*scf++) << shift)];
                        step = 3;
                    }
                }
                const int y0 = huff_pair(d, h->tree, &part2remain);
                const int x = y0 >> 4, y = y0 & 0xF;
                if (x) max[lwin] = cb;
                PUT(lin_value(d, x, h->linbits, v, &part2remain));
                xrpnt += step;
                if (y) max[lwin] = cb;
                PUT(lin_value(d, y, h->linbits, v, &part2remain));
                xrpnt += step;
            }
        }
        for (; l3 > 0 && part2remain > 0; l3--) {
            const int16_t* val = mp3_htc[g->count1table_select];
            int16_t a;
            while ((a = *val++) < 0) {
                part2remain--;
                if (part2remain < 0) {
                    part2remain++;
                    a = 0;
                    break;
                }
                if (get1bit(d)) val -= a;
            }
            for (int i = 0; i < 4; i++) {
                if (!(i & 1)) {
                    if (!mc) {
                        if (m >= me) {
                            d->bad = 1;
                            return;
                        }
                        mc = *m++;
                        xrpnt = ((float*)xr) + (*m++);
                        lwin = *m++;
                        cb = *m++;
                        if (lwin == 3) {
                            v = g_gainpow2[g->pow2gain + ((*scf++) << shift)];
                            step = 1;
                        } else {
                            v = g_gainpow2[g->full_gain[lwin] + ((*scf++) << shift)];
                            step = 3;
                        }
                    }
                    mc--;
                }
                float xv = 0.0f;
                if (a & (0x8 >> i)) {
                    max[lwin] = cb;
                    part2remain--;
                    if (part2remain < 0) {
                        part2remain++;
                        break;
                    }
                    xv = get1bit(d) ? -v : v;
                }
                PUT(xv);
                xrpnt += step;
            }
        }
        while (m < me) {
            if (!mc) {
                mc = *m++;
                xrpnt = ((float*)xr) + *m++;
                step = (*m++) == 3 ? 1 : 3;
                m++;
            }
            mc--;
            PUT(0.0f);
            xrpnt += step;
            PUT(0.0f);
            xrpnt += step;
        }
        g->maxband[0] = (unsigned)(max[0] + 1);
        g->maxband[1] = (unsigned)(max[1] + 1);
        g->maxband[2] = (unsigned)(max[2] + 1);
        g->maxbandl = (unsigned)(max[3] + 1);
        {
            int rmax = max[0] > max[1] ? max[0] : max[1];
            rmax = (rmax > max[2] ? rmax : max[2]) + 1;
            g->maxb = (unsigned)(rmax ? g_shortLimit[sfreq][rmax] : g_longLimit[sfreq][max[3] + 1]);
        }
    } else {
        const int* pretab = g->preflag ? g_pretab1 : g_pretab2;
        int max = -1, cb = 0, mc = 0;
        int* m = g_map[sfreq][2];
        float v = 0.0f;
        for (int i = 0; i < 3; i++) {
            int lp = l[i];
            const mp3_huffman_table* h = &mp3_ht[g->table_select[i]];
            for (; lp; lp--, mc--) {
                if (!mc) {
                    mc = *m++;
                    v = g_gainpow2[g->pow2gain + (((*scf++) + (*pretab++)) << shift)];
                    cb = *m++;
                    if (cb == 21) v = 0.0f; /* the module's band 21 (no scale factor of its own) is silent */
                }
                const int y0 = huff_pair(d, h->tree, &part2remain);
                const int x = y0 >> 4, y = y0 & 0xF;
                if (x) max = cb;
                PUT(lin_value(d, x, h->linbits, v, &part2remain));
                xrpnt++;
                if (y) max = cb;
                PUT(lin_value(d, y, h->linbits, v, &part2remain));
                xrpnt++;
            }
        }
        for (; l3 && part2remain > 0; l3--) {
            const int16_t* val = mp3_htc[g->count1table_select];
            int16_t a;
            while ((a = *val++) < 0) {
                part2remain--;
                if (part2remain < 0) {
                    part2remain++;
                    a = 0;
                    break;
                }
                if (get1bit(d)) val -= a;
            }
            for (int i = 0; i < 4; i++) {
                if (!(i & 1)) {
                    if (!mc) {
                        mc = *m++;
                        cb = *m++;
                        v = g_gainpow2[g->pow2gain + (((*scf++) + (*pretab++)) << shift)];
                        if (cb == 21) v = 0.0f;
                    }
                    mc--;
                }
                float xv = 0.0f;
                if (a & (0x8 >> i)) {
                    max = cb;
                    part2remain--;
                    if (part2remain < 0) {
                        part2remain++;
                        break;
                    }
                    xv = get1bit(d) ? -v : v;
                }
                PUT(xv);
                xrpnt++;
            }
        }
        g->maxbandl = (unsigned)(max + 1);
        g->maxb = (unsigned)g_longLimit[sfreq][g->maxbandl];
    }
#undef PUT
    while (part2remain > 16) {
        getbits(d, 16);
        part2remain -= 16;
    }
    if (part2remain > 0) getbits(d, part2remain);
    else if (part2remain < 0) d->bad = 1; /* mpglib stops the frame here; what the module does is not known */
}

/* ---- antialias, IMDCT (mpglib III_antialias, dct36, dct12, III_hybrid) ---- */

static void antialias(float xr[SBLIMIT][SSLIMIT], const gr_info* g)
{
    int sblim;
    if (g->block_type == 2) {
        if (!g->mixed_block_flag) return;
        sblim = 1;
    } else {
        sblim = (int)g->maxb - 1;
    }
    float* xr1 = (float*)xr[1];
    for (int sb = sblim; sb > 0; sb--, xr1 += 10) {
        const float *cs = g_aa_cs, *ca = g_aa_ca;
        float* xr2 = xr1;
        for (int ss = 7; ss >= 0; ss--) {
            const float bu = *--xr2, bd = *xr1;
            *xr2 = (bu * (*cs)) - (bd * (*ca));
            *xr1++ = (bd * (*cs++)) + (bu * (*ca++));
        }
    }
}

static void dct36(const float* inbuf, const float* o1, float* o2, const float* wintab, float* tsbuf)
{
    /* mpglib sums its input in place; the module leaves the granule's spectrum as it was */
    float in[18];
    memcpy(in, inbuf, sizeof in);
    in[17] += in[16];
    in[16] += in[15];
    in[15] += in[14];
    in[14] += in[13];
    in[13] += in[12];
    in[12] += in[11];
    in[11] += in[10];
    in[10] += in[9];
    in[9] += in[8];
    in[8] += in[7];
    in[7] += in[6];
    in[6] += in[5];
    in[5] += in[4];
    in[4] += in[3];
    in[3] += in[2];
    in[2] += in[1];
    in[1] += in[0];
    in[17] += in[15];
    in[15] += in[13];
    in[13] += in[11];
    in[11] += in[9];
    in[9] += in[7];
    in[7] += in[5];
    in[5] += in[3];
    in[3] += in[1];
    {
        const float* c = g_COS9;
        float* out2 = o2;
        const float* w = wintab;
        const float* out1 = o1;
        float* ts = tsbuf;
#define MACRO0(v)                                                                                                      \
    {                                                                                                                  \
        float tmp;                                                                                                     \
        out2[9 + (v)] = (tmp = sum0 + sum1) * w[27 + (v)];                                                             \
        out2[8 - (v)] = tmp * w[26 - (v)];                                                                             \
    }                                                                                                                  \
    sum0 -= sum1;                                                                                                      \
    ts[SBLIMIT * (8 - (v))] = out1[8 - (v)] + sum0 * w[8 - (v)];                                                       \
    ts[SBLIMIT * (9 + (v))] = out1[9 + (v)] + sum0 * w[9 + (v)];
#define MACRO1(v)                                                                                                      \
    {                                                                                                                  \
        float sum0, sum1;                                                                                              \
        sum0 = tmp1a + tmp2a;                                                                                          \
        sum1 = (tmp1b + tmp2b) * g_tfcos36[(v)];                                                                       \
        MACRO0(v);                                                                                                     \
    }
#define MACRO2(v)                                                                                                      \
    {                                                                                                                  \
        float sum0, sum1;                                                                                              \
        sum0 = tmp2a - tmp1a;                                                                                          \
        sum1 = (tmp2b - tmp1b) * g_tfcos36[(v)];                                                                       \
        MACRO0(v);                                                                                                     \
    }
        const float ta33 = in[2 * 3 + 0] * c[3], ta66 = in[2 * 6 + 0] * c[6];
        const float tb33 = in[2 * 3 + 1] * c[3], tb66 = in[2 * 6 + 1] * c[6];
        {
            const float tmp1a = in[2 * 1 + 0] * c[1] + ta33 + in[2 * 5 + 0] * c[5] + in[2 * 7 + 0] * c[7];
            const float tmp1b = in[2 * 1 + 1] * c[1] + tb33 + in[2 * 5 + 1] * c[5] + in[2 * 7 + 1] * c[7];
            const float tmp2a = in[2 * 0 + 0] + in[2 * 2 + 0] * c[2] + in[2 * 4 + 0] * c[4] + ta66 + in[2 * 8 + 0] * c[8];
            const float tmp2b = in[2 * 0 + 1] + in[2 * 2 + 1] * c[2] + in[2 * 4 + 1] * c[4] + tb66 + in[2 * 8 + 1] * c[8];
            MACRO1(0);
            MACRO2(8);
        }
        {
            const float tmp1a = (in[2 * 1 + 0] - in[2 * 5 + 0] - in[2 * 7 + 0]) * c[3];
            const float tmp1b = (in[2 * 1 + 1] - in[2 * 5 + 1] - in[2 * 7 + 1]) * c[3];
            const float tmp2a = (in[2 * 2 + 0] - in[2 * 4 + 0] - in[2 * 8 + 0]) * c[6] - in[2 * 6 + 0] + in[2 * 0 + 0];
            const float tmp2b = (in[2 * 2 + 1] - in[2 * 4 + 1] - in[2 * 8 + 1]) * c[6] - in[2 * 6 + 1] + in[2 * 0 + 1];
            MACRO1(1);
            MACRO2(7);
        }
        {
            const float tmp1a = in[2 * 1 + 0] * c[5] - ta33 - in[2 * 5 + 0] * c[7] + in[2 * 7 + 0] * c[1];
            const float tmp1b = in[2 * 1 + 1] * c[5] - tb33 - in[2 * 5 + 1] * c[7] + in[2 * 7 + 1] * c[1];
            const float tmp2a = in[2 * 0 + 0] - in[2 * 2 + 0] * c[8] - in[2 * 4 + 0] * c[2] + ta66 + in[2 * 8 + 0] * c[4];
            const float tmp2b = in[2 * 0 + 1] - in[2 * 2 + 1] * c[8] - in[2 * 4 + 1] * c[2] + tb66 + in[2 * 8 + 1] * c[4];
            MACRO1(2);
            MACRO2(6);
        }
        {
            const float tmp1a = in[2 * 1 + 0] * c[7] - ta33 + in[2 * 5 + 0] * c[1] - in[2 * 7 + 0] * c[5];
            const float tmp1b = in[2 * 1 + 1] * c[7] - tb33 + in[2 * 5 + 1] * c[1] - in[2 * 7 + 1] * c[5];
            const float tmp2a = in[2 * 0 + 0] - in[2 * 2 + 0] * c[4] + in[2 * 4 + 0] * c[8] + ta66 - in[2 * 8 + 0] * c[2];
            const float tmp2b = in[2 * 0 + 1] - in[2 * 2 + 1] * c[4] + in[2 * 4 + 1] * c[8] + tb66 - in[2 * 8 + 1] * c[2];
            MACRO1(3);
            MACRO2(5);
        }
        {
            float sum0 = in[2 * 0 + 0] - in[2 * 2 + 0] + in[2 * 4 + 0] - in[2 * 6 + 0] + in[2 * 8 + 0];
            const float sum1 = (in[2 * 0 + 1] - in[2 * 2 + 1] + in[2 * 4 + 1] - in[2 * 6 + 1] + in[2 * 8 + 1]) * g_tfcos36[4];
            MACRO0(4);
        }
#undef MACRO0
#undef MACRO1
#undef MACRO2
    }
}

static void dct12(const float* in, const float* rawout1, float* rawout2, const float* wi, float* ts)
{
#define DCT12_PART1                                                                                                    \
    in5 = in[5 * 3];                                                                                                   \
    in5 += (in4 = in[4 * 3]);                                                                                          \
    in4 += (in3 = in[3 * 3]);                                                                                          \
    in3 += (in2 = in[2 * 3]);                                                                                          \
    in2 += (in1 = in[1 * 3]);                                                                                          \
    in1 += (in0 = in[0 * 3]);                                                                                          \
    in5 += in3;                                                                                                        \
    in3 += in1;                                                                                                        \
    in2 *= g_COS6_1;                                                                                                   \
    in3 *= g_COS6_1;
#define DCT12_PART2                                                                                                    \
    in0 += in4 * g_COS6_2;                                                                                             \
    in4 = in0 + in2;                                                                                                   \
    in0 -= in2;                                                                                                        \
    in1 += in5 * g_COS6_2;                                                                                             \
    in5 = (in1 + in3) * g_tfcos12[0];                                                                                  \
    in1 = (in1 - in3) * g_tfcos12[2];                                                                                  \
    in3 = in4 + in5;                                                                                                   \
    in4 -= in5;                                                                                                        \
    in2 = in0 + in1;                                                                                                   \
    in0 -= in1;
    {
        float in0, in1, in2, in3, in4, in5;
        const float* out1 = rawout1;
        ts[SBLIMIT * 0] = out1[0];
        ts[SBLIMIT * 1] = out1[1];
        ts[SBLIMIT * 2] = out1[2];
        ts[SBLIMIT * 3] = out1[3];
        ts[SBLIMIT * 4] = out1[4];
        ts[SBLIMIT * 5] = out1[5];
        DCT12_PART1
        {
            float tmp0, tmp1 = (in0 - in4);
            {
                const float tmp2 = (in1 - in5) * g_tfcos12[1];
                tmp0 = tmp1 + tmp2;
                tmp1 -= tmp2;
            }
            ts[(17 - 1) * SBLIMIT] = out1[17 - 1] + tmp0 * wi[11 - 1];
            ts[(12 + 1) * SBLIMIT] = out1[12 + 1] + tmp0 * wi[6 + 1];
            ts[(6 + 1) * SBLIMIT] = out1[6 + 1] + tmp1 * wi[1];
            ts[(11 - 1) * SBLIMIT] = out1[11 - 1] + tmp1 * wi[5 - 1];
        }
        DCT12_PART2
        ts[(17 - 0) * SBLIMIT] = out1[17 - 0] + in2 * wi[11 - 0];
        ts[(12 + 0) * SBLIMIT] = out1[12 + 0] + in2 * wi[6 + 0];
        ts[(12 + 2) * SBLIMIT] = out1[12 + 2] + in3 * wi[6 + 2];
        ts[(17 - 2) * SBLIMIT] = out1[17 - 2] + in3 * wi[11 - 2];
        ts[(6 + 0) * SBLIMIT] = out1[6 + 0] + in0 * wi[0];
        ts[(11 - 0) * SBLIMIT] = out1[11 - 0] + in0 * wi[5 - 0];
        ts[(6 + 2) * SBLIMIT] = out1[6 + 2] + in4 * wi[2];
        ts[(11 - 2) * SBLIMIT] = out1[11 - 2] + in4 * wi[5 - 2];
    }
    in++;
    {
        float in0, in1, in2, in3, in4, in5;
        float* out2 = rawout2;
        DCT12_PART1
        {
            float tmp0, tmp1 = (in0 - in4);
            {
                const float tmp2 = (in1 - in5) * g_tfcos12[1];
                tmp0 = tmp1 + tmp2;
                tmp1 -= tmp2;
            }
            out2[5 - 1] = tmp0 * wi[11 - 1];
            out2[0 + 1] = tmp0 * wi[6 + 1];
            ts[(12 + 1) * SBLIMIT] += tmp1 * wi[1];
            ts[(17 - 1) * SBLIMIT] += tmp1 * wi[5 - 1];
        }
        DCT12_PART2
        out2[5 - 0] = in2 * wi[11 - 0];
        out2[0 + 0] = in2 * wi[6 + 0];
        out2[0 + 2] = in3 * wi[6 + 2];
        out2[5 - 2] = in3 * wi[11 - 2];
        ts[(12 + 0) * SBLIMIT] += in0 * wi[0];
        ts[(17 - 0) * SBLIMIT] += in0 * wi[5 - 0];
        ts[(12 + 2) * SBLIMIT] += in4 * wi[2];
        ts[(17 - 2) * SBLIMIT] += in4 * wi[5 - 2];
    }
    in++;
    {
        float in0, in1, in2, in3, in4, in5;
        float* out2 = rawout2;
        out2[12] = out2[13] = out2[14] = out2[15] = out2[16] = out2[17] = 0.0f;
        DCT12_PART1
        {
            float tmp0, tmp1 = (in0 - in4);
            {
                const float tmp2 = (in1 - in5) * g_tfcos12[1];
                tmp0 = tmp1 + tmp2;
                tmp1 -= tmp2;
            }
            out2[11 - 1] = tmp0 * wi[11 - 1];
            out2[6 + 1] = tmp0 * wi[6 + 1];
            out2[0 + 1] += tmp1 * wi[1];
            out2[5 - 1] += tmp1 * wi[5 - 1];
        }
        DCT12_PART2
        out2[11 - 0] = in2 * wi[11 - 0];
        out2[6 + 0] = in2 * wi[6 + 0];
        out2[6 + 2] = in3 * wi[6 + 2];
        out2[11 - 2] = in3 * wi[11 - 2];
        out2[0 + 0] += in0 * wi[0];
        out2[5 - 0] += in0 * wi[5 - 0];
        out2[0 + 2] += in4 * wi[2];
        out2[5 - 2] += in4 * wi[5 - 2];
    }
#undef DCT12_PART1
#undef DCT12_PART2
}

/* The module's III_hybrid: the old block in, a new one (heap) out, copied over the old one. */
static void hybrid(mp3_state* d, float fsIn[SBLIMIT][SSLIMIT], float tsOut[SSLIMIT][SBLIMIT], int ch, const gr_info* g)
{
    float* tspnt = (float*)tsOut;
    const float* rawout1 = d->block[ch];
    float* rawout2 = d->newblock;
    int sb = 0;
    if (g->mixed_block_flag) {
        sb = 2;
        dct36(fsIn[0], rawout1, rawout2, g_win[0], tspnt);
        dct36(fsIn[1], rawout1 + 18, rawout2 + 18, g_win1[0], tspnt + 1);
        rawout1 += 36;
        rawout2 += 36;
        tspnt += 2;
    }
    const unsigned bt = g->block_type;
    if (bt == 2) {
        for (; sb < (int)g->maxb; sb += 2, tspnt += 2, rawout1 += 36, rawout2 += 36) {
            dct12(fsIn[sb], rawout1, rawout2, g_win[2], tspnt);
            dct12(fsIn[sb + 1], rawout1 + 18, rawout2 + 18, g_win1[2], tspnt + 1);
        }
    } else {
        for (; sb < (int)g->maxb; sb += 2, tspnt += 2, rawout1 += 36, rawout2 += 36) {
            dct36(fsIn[sb], rawout1, rawout2, g_win[bt], tspnt);
            dct36(fsIn[sb + 1], rawout1 + 18, rawout2 + 18, g_win1[bt], tspnt + 1);
        }
    }
    for (; sb < SBLIMIT; sb++, tspnt++) {
        for (int i = 0; i < SSLIMIT; i++) {
            tspnt[i * SBLIMIT] = *rawout1++;
            *rawout2++ = 0.0f;
        }
    }
    memcpy(d->block[ch], d->newblock, sizeof d->newblock);
}

/* ---- the polyphase filterbank (mpglib dct64_i386.c dct64, decode_i386.c synth_1to1; the module's DCT64 and
 * synth_filterbank: 16-bit samples held to +-32767 and truncated, 32 to a channel's plane) ---- */

static void dct64(float* out0, float* out1, const float* samples)
{
    float b1[0x20], b2[0x20];
    {
        const float* costab = g_cos64;
        for (int i = 0; i < 16; i++) {
            b1[i] = samples[i] + samples[31 - i];
            b1[31 - i] = (samples[i] - samples[31 - i]) * costab[i];
        }
    }
    {
        const float* costab = g_cos32;
        for (int i = 0; i < 8; i++) {
            b2[i] = b1[i] + b1[15 - i];
            b2[15 - i] = (b1[i] - b1[15 - i]) * costab[i];
            b2[16 + i] = b1[16 + i] + b1[31 - i];
            b2[31 - i] = (b1[31 - i] - b1[16 + i]) * costab[i];
        }
    }
    {
        const float* costab = g_cos16;
        for (int base = 0; base < 32; base += 16) {
            for (int i = 0; i < 4; i++) {
                b1[base + i] = b2[base + i] + b2[base + 7 - i];
                b1[base + 7 - i] = (b2[base + i] - b2[base + 7 - i]) * costab[i];
                b1[base + 8 + i] = b2[base + 8 + i] + b2[base + 15 - i];
                b1[base + 15 - i] = (b2[base + 15 - i] - b2[base + 8 + i]) * costab[i];
            }
        }
    }
    {
        const float cos0 = g_cos8[0], cos1 = g_cos8[1];
        for (int base = 0; base < 32; base += 8) {
            b2[base + 0] = b1[base + 0] + b1[base + 3];
            b2[base + 3] = (b1[base + 0] - b1[base + 3]) * cos0;
            b2[base + 1] = b1[base + 1] + b1[base + 2];
            b2[base + 2] = (b1[base + 1] - b1[base + 2]) * cos1;
            b2[base + 4] = b1[base + 4] + b1[base + 7];
            b2[base + 7] = (b1[base + 7] - b1[base + 4]) * cos0;
            b2[base + 5] = b1[base + 5] + b1[base + 6];
            b2[base + 6] = (b1[base + 6] - b1[base + 5]) * cos1;
        }
    }
    {
        const float cos0 = g_cos4[0];
        for (int base = 0; base < 32; base += 8) {
            b1[base + 0] = b2[base + 0] + b2[base + 1];
            b1[base + 1] = (b2[base + 0] - b2[base + 1]) * cos0;
            b1[base + 2] = b2[base + 2] + b2[base + 3];
            b1[base + 3] = (b2[base + 3] - b2[base + 2]) * cos0;
            b1[base + 2] += b1[base + 3];
            b1[base + 4] = b2[base + 4] + b2[base + 5];
            b1[base + 5] = (b2[base + 4] - b2[base + 5]) * cos0;
            b1[base + 6] = b2[base + 6] + b2[base + 7];
            b1[base + 7] = (b2[base + 7] - b2[base + 6]) * cos0;
            b1[base + 6] += b1[base + 7];
            b1[base + 4] += b1[base + 6];
            b1[base + 6] += b1[base + 5];
            b1[base + 5] += b1[base + 7];
        }
    }
    out0[0x10 * 16] = b1[0x00];
    out0[0x10 * 12] = b1[0x04];
    out0[0x10 * 8] = b1[0x02];
    out0[0x10 * 4] = b1[0x06];
    out0[0x10 * 0] = b1[0x01];
    out1[0x10 * 0] = b1[0x01];
    out1[0x10 * 4] = b1[0x05];
    out1[0x10 * 8] = b1[0x03];
    out1[0x10 * 12] = b1[0x07];
    b1[0x08] += b1[0x0C];
    out0[0x10 * 14] = b1[0x08];
    b1[0x0C] += b1[0x0a];
    out0[0x10 * 10] = b1[0x0C];
    b1[0x0A] += b1[0x0E];
    out0[0x10 * 6] = b1[0x0A];
    b1[0x0E] += b1[0x09];
    out0[0x10 * 2] = b1[0x0E];
    b1[0x09] += b1[0x0D];
    out1[0x10 * 2] = b1[0x09];
    b1[0x0D] += b1[0x0B];
    out1[0x10 * 6] = b1[0x0D];
    b1[0x0B] += b1[0x0F];
    out1[0x10 * 10] = b1[0x0B];
    out1[0x10 * 14] = b1[0x0F];
    b1[0x18] += b1[0x1C];
    out0[0x10 * 15] = b1[0x10] + b1[0x18];
    out0[0x10 * 13] = b1[0x18] + b1[0x14];
    b1[0x1C] += b1[0x1a];
    out0[0x10 * 11] = b1[0x14] + b1[0x1C];
    out0[0x10 * 9] = b1[0x1C] + b1[0x12];
    b1[0x1A] += b1[0x1E];
    out0[0x10 * 7] = b1[0x12] + b1[0x1A];
    out0[0x10 * 5] = b1[0x1A] + b1[0x16];
    b1[0x1E] += b1[0x19];
    out0[0x10 * 3] = b1[0x16] + b1[0x1E];
    out0[0x10 * 1] = b1[0x1E] + b1[0x11];
    b1[0x19] += b1[0x1D];
    out1[0x10 * 1] = b1[0x11] + b1[0x19];
    out1[0x10 * 3] = b1[0x19] + b1[0x15];
    b1[0x1D] += b1[0x1B];
    out1[0x10 * 5] = b1[0x15] + b1[0x1D];
    out1[0x10 * 7] = b1[0x1D] + b1[0x13];
    b1[0x1B] += b1[0x1F];
    out1[0x10 * 9] = b1[0x13] + b1[0x1B];
    out1[0x10 * 11] = b1[0x1B] + b1[0x17];
    out1[0x10 * 13] = b1[0x17] + b1[0x1F];
    out1[0x10 * 15] = b1[0x1F];
}

static inline int16_t pcm_sample(float sum)
{
    if (sum > 32767.0f) sum = 32767.0f;
    if (sum < -32767.0f) sum = -32767.0f;
    return (int16_t)(int32_t)sum; /* toward zero, as cflts */
}

/* The window for one channel's 32 samples (synth_1to1 after its dct64), from buffs[ch] and the current bo. */
static void synth_window(mp3_state* d, int ch, int16_t* out)
{
    float(*buf)[0x110] = d->buffs[ch];
    const int bo = d->bo;
    const float* b0;
    int bo1;
    if (bo & 1) {
        b0 = buf[0];
        bo1 = bo;
    } else {
        b0 = buf[1];
        bo1 = bo + 1;
    }
    const float* window = g_decwin + 16 - bo1;
    for (int j = 16; j; j--, b0 += 0x10, window += 0x20) {
        float sum = window[0x0] * b0[0x0];
        sum -= window[0x1] * b0[0x1];
        sum += window[0x2] * b0[0x2];
        sum -= window[0x3] * b0[0x3];
        sum += window[0x4] * b0[0x4];
        sum -= window[0x5] * b0[0x5];
        sum += window[0x6] * b0[0x6];
        sum -= window[0x7] * b0[0x7];
        sum += window[0x8] * b0[0x8];
        sum -= window[0x9] * b0[0x9];
        sum += window[0xA] * b0[0xA];
        sum -= window[0xB] * b0[0xB];
        sum += window[0xC] * b0[0xC];
        sum -= window[0xD] * b0[0xD];
        sum += window[0xE] * b0[0xE];
        sum -= window[0xF] * b0[0xF];
        *out++ = pcm_sample(sum);
    }
    {
        float sum = window[0x0] * b0[0x0];
        sum += window[0x2] * b0[0x2];
        sum += window[0x4] * b0[0x4];
        sum += window[0x6] * b0[0x6];
        sum += window[0x8] * b0[0x8];
        sum += window[0xA] * b0[0xA];
        sum += window[0xC] * b0[0xC];
        sum += window[0xE] * b0[0xE];
        *out++ = pcm_sample(sum);
        b0 -= 0x10;
        window -= 0x20;
    }
    window += bo1 << 1;
    for (int j = 15; j; j--, b0 -= 0x10, window -= 0x20) {
        float sum = -window[-0x1] * b0[0x0];
        sum -= window[-0x2] * b0[0x1];
        sum -= window[-0x3] * b0[0x2];
        sum -= window[-0x4] * b0[0x3];
        sum -= window[-0x5] * b0[0x4];
        sum -= window[-0x6] * b0[0x5];
        sum -= window[-0x7] * b0[0x6];
        sum -= window[-0x8] * b0[0x7];
        sum -= window[-0x9] * b0[0x8];
        sum -= window[-0xA] * b0[0x9];
        sum -= window[-0xB] * b0[0xA];
        sum -= window[-0xC] * b0[0xB];
        sum -= window[-0xD] * b0[0xC];
        sum -= window[-0xE] * b0[0xD];
        sum -= window[-0xF] * b0[0xE];
        sum -= window[-0x0] * b0[0xF];
        *out++ = pcm_sample(sum);
    }
}

static void dct64_channel(mp3_state* d, int ch, const float* band)
{
    float(*buf)[0x110] = d->buffs[ch];
    const int bo = d->bo;
    if (bo & 1) dct64(buf[1] + ((bo + 1) & 0xF), buf[0] + bo, band);
    else dct64(buf[0] + bo, buf[1] + bo + 1, band);
}

/* ---- the module's decodePacket ---- */

static void decode_packet(mp3_state* d)
{
    const mp3_frame* fr = &d->fr;
    const int stereo = fr->stereo, sfreq = fr->sampling_frequency;
    int ms_stereo = 0, i_stereo = 0;
    if (fr->mode == 1) {
        ms_stereo = fr->mode_ext & 0x2;
        i_stereo = fr->mode_ext & 0x1;
    }
    if (i_stereo) { /* the module prints "Intensity Stereo" not supported and fails */
        d->bad = 1;
        return;
    }
    get_side_info(d, stereo, ms_stereo, sfreq);
    if (d->bad) return;
    /* set_pointer: the reservoir before the main data (mpglib's set_pointer over the module's backup) */
    {
        const unsigned backstep = d->si.main_data_begin;
        d->wp = 512 + (uint32_t)d->ssize - backstep;
        /* the bytes come from the backup the caller kept: bs[0..512) holds it already (see decode_frame) */
        d->bi = 0;
    }
    for (int gr = 0; gr < (fr->lsf ? 1 : 2); gr++) {
        {
            gr_info* g = &d->si.ch[0].gr[gr];
            const int part2bits = fr->lsf ? get_scale_factors_2(d, d->scalefacs[0], g, 0)
                                          : get_scale_factors_1(d, d->scalefacs[0], g);
            dequantize_sample(d, d->hybridIn[0], d->scalefacs[0], g, sfreq, part2bits);
        }
        if (stereo == 2) {
            gr_info* g = &d->si.ch[1].gr[gr];
            const int part2bits = fr->lsf ? get_scale_factors_2(d, d->scalefacs[1], g, i_stereo)
                                          : get_scale_factors_1(d, d->scalefacs[1], g);
            dequantize_sample(d, d->hybridIn[1], d->scalefacs[1], g, sfreq, part2bits);
            if (ms_stereo) {
                gr_info* g0 = &d->si.ch[0].gr[gr];
                const unsigned maxb = g0->maxb > g->maxb ? g0->maxb : g->maxb;
                float* in0 = (float*)d->hybridIn[0];
                float* in1 = (float*)d->hybridIn[1];
                for (unsigned i = 0; i < SSLIMIT * maxb; i++) {
                    const float tmp0 = in0[i], tmp1 = in1[i];
                    in0[i] = tmp0 + tmp1;
                    in1[i] = tmp0 - tmp1;
                }
                if (g->maxb > g0->maxb) g0->maxb = g->maxb;
                else g->maxb = g0->maxb;
            }
        }
        if (d->bad) return;
        for (int ch = 0; ch < stereo; ch++) {
            const gr_info* g = &d->si.ch[ch].gr[gr];
            antialias(d->hybridIn[ch], g);
            hybrid(d, d->hybridIn[ch], d->hybridOut[ch], ch, g);
        }
        for (int ss = 0; ss < SSLIMIT; ss++) {
            d->bo = (d->bo - 1) & 0xF;
            dct64_channel(d, 0, d->hybridOut[0][ss]);
            if (stereo == 2) dct64_channel(d, 1, d->hybridOut[1][ss]);
            synth_window(d, 0, d->pcm[0] + d->pcm_point / 2);
            if (stereo == 2) synth_window(d, 1, d->pcm[1] + d->pcm_point / 2);
            d->pcm_point += 64;
        }
    }
}

/* ---- decodeFrame ---- */

static inline uint32_t pref(const SPUContext* ctx, int r) { return ctx->gpr[r].u32[3]; }
static inline int in_ls(uint32_t start, uint32_t size) { return start <= PS_SPU_LS_SIZE && size <= PS_SPU_LS_SIZE - start; }

static void ld_floats(float* dst, const uint8_t* src, int n)
{
    for (int i = 0; i < n; i++) dst[i] = ps_ldf32(src + 4 * i);
}
static void st_floats(uint8_t* dst, const float* src, int n)
{
    for (int i = 0; i < n; i++) ps_stf32(dst + 4 * i, src[i]);
}
/* A word of a module variable's quadword (the module inserts words: the rest of the quadword stays). */
static inline void st_var(uint8_t* ls, uint32_t mod, uint32_t var, uint32_t v) { ps_st32(ls + ((mod + var) & LS_MASK), v); }

/* The header as the module's decode_header reads it; 0: what the module refuses (and prints about) or what this code
 * does not do as the module does. */
static int decode_header(mp3_frame* fr, uint32_t h, int* ssize)
{
    if ((h & 0xFFE00000u) != 0xFFE00000u) return 0;
    if (h & (1u << 20)) {
        fr->lsf = (int)(((h >> 19) & 1) ^ 1);
        fr->mpeg25 = 0;
    } else {
        fr->lsf = 1;
        fr->mpeg25 = 1;
    }
    fr->lay = 4 - (int)((h >> 17) & 3);
    if (((h >> 10) & 3) == 3) return 0;  /* reserved rate: the module reads past its tables */
    if (fr->lay != 3) return 0;          /* the module decodes any layer as layer III */
    if (fr->mpeg25 && !fr->lsf) return 0;
    fr->sampling_frequency = (int)((h >> 10) & 3) + (fr->mpeg25 ? 6 : fr->lsf * 3);
    fr->error_protection = (int)(((h >> 16) & 1) ^ 1);
    fr->bitrate_index = (int)((h >> 12) & 0xF);
    fr->padding = (int)((h >> 9) & 1);
    fr->extension = (int)((h >> 8) & 1);
    fr->mode = (int)((h >> 6) & 3);
    fr->mode_ext = (int)((h >> 4) & 3);
    fr->copyright = (int)((h >> 3) & 1);
    fr->original = (int)((h >> 2) & 1);
    fr->emphasis = (int)(h & 3);
    fr->stereo = fr->mode == 3 ? 1 : 2;
    if (!fr->bitrate_index || fr->bitrate_index == 15) return 0;
    if (fr->lsf) *ssize = fr->stereo == 1 ? 9 : 17;
    else *ssize = fr->stereo == 1 ? 17 : 32;
    if (fr->error_protection) *ssize += 2;
    fr->framesize = g_tabsel3[fr->lsf][fr->bitrate_index] * 144000 / (g_freqs[fr->sampling_frequency] << fr->lsf);
    fr->framesize += fr->padding - 4;
    if (fr->framesize < 0 || fr->framesize > 4096) return 0;
    return 1;
}

/* The module's tables its init_layer3 points its variables at (image vaddrs). */
enum {
    V_tab0 = 0x9600, V_tab1 = 0x9610, V_tab2 = 0x9620, V_tab3 = 0x9650, V_tab5 = 0x9680, V_tab6 = 0x96C0,
    V_tab7 = 0x9700, V_tab8 = 0x9790, V_tab9 = 0x9820, V_tab10 = 0x98B0, V_tab11 = 0x99B0, V_tab12 = 0x9AB0,
    V_tab13 = 0x9BB0, V_tab15 = 0x9FB0, V_tab16 = 0xA3B0, V_tab24 = 0xA7B0, V_tab_c0 = 0xABB0, V_tab_c1 = 0xABF0,
    V_mapCnt0 = 0xB6E0, V_mapIndex0 = 0xBC40, V_mapLwin0 = 0xC1A0, V_mapCB0 = 0xC240, V_mapCnt1 = 0xC2E0,
    V_mapIndex1 = 0xC860, V_mapLwin1 = 0xCDE0, V_mapCB1 = 0xCE80,
    /* variables (each in a quadword of its own; the bytes past a variable's size are padding no code reads) */
    V_mapCB = 0x10030, V_mapCnt = 0x10050, V_ht = 0x100B0, V_mapLwin = 0x101B0, V_htc = 0x10220, V_mapIndex = 0x10230,
    V_mapSize = 0x102A0,
};

/* An address in the module's image as a word: module + vaddr - 0x80. */
static inline uint32_t mod_addr(uint32_t mod, uint32_t vaddr) { return (mod + vaddr - MOD_V0) & LS_MASK; }

/* What init_layer3 (run by every decodeFrame through inputMP3System) writes: mpglib's ht/htc, and the map tables of
 * the module's dequantisation by rate (counts, indices and windows of each band, for long-and-short and short
 * blocks). */
static void init_layer3_vars(uint8_t* ls, uint32_t mod)
{
    static const uint16_t tabs[32] = {V_tab0, V_tab1, V_tab2, V_tab3, V_tab0, V_tab5, V_tab6, V_tab7, V_tab8, V_tab9,
        V_tab10, V_tab11, V_tab12, V_tab13, V_tab0, V_tab15, V_tab16, V_tab16, V_tab16, V_tab16, V_tab16, V_tab16,
        V_tab16, V_tab16, V_tab24, V_tab24, V_tab24, V_tab24, V_tab24, V_tab24, V_tab24, V_tab24};
    for (uint32_t i = 0; i < 32; i++) {
        st_var(ls, mod, V_ht - MOD_V0 + 8 * i, mp3_ht[i].linbits);
        st_var(ls, mod, V_ht - MOD_V0 + 8 * i + 4, mod_addr(mod, tabs[i]));
    }
    st_var(ls, mod, V_htc - MOD_V0, 0);
    st_var(ls, mod, V_htc - MOD_V0 + 4, mod_addr(mod, V_tab_c0));
    st_var(ls, mod, V_htc - MOD_V0 + 8, 0);
    st_var(ls, mod, V_htc - MOD_V0 + 12, mod_addr(mod, V_tab_c1));
    st_var(ls, mod, V_mapCB - MOD_V0, mod_addr(mod, V_mapCB0));
    st_var(ls, mod, V_mapCB - MOD_V0 + 4, mod_addr(mod, V_mapCB1));
    for (uint32_t k = 0; k < 9; k++) {
        st_var(ls, mod, V_mapCnt - MOD_V0 + 8 * k, mod_addr(mod, V_mapCnt0) + 152 * k);
        st_var(ls, mod, V_mapCnt - MOD_V0 + 8 * k + 4, mod_addr(mod, V_mapCnt1) + 156 * k);
        st_var(ls, mod, V_mapIndex - MOD_V0 + 8 * k, mod_addr(mod, V_mapIndex0) + 152 * k);
        st_var(ls, mod, V_mapIndex - MOD_V0 + 8 * k + 4, mod_addr(mod, V_mapIndex1) + 156 * k);
        st_var(ls, mod, V_mapLwin - MOD_V0 + 8 * k, mod_addr(mod, V_mapLwin0));
        st_var(ls, mod, V_mapLwin - MOD_V0 + 8 * k + 4, mod_addr(mod, V_mapLwin1));
    }
    st_var(ls, mod, V_mapSize - MOD_V0, 38);
    st_var(ls, mod, V_mapSize - MOD_V0 + 4, 39);
}

/* A 16-byte variable (the timers): the decrementer in its first word, the rest zero (rdch's register). */
static void st_quad1(uint8_t* ls, uint32_t mod, uint32_t var, uint32_t v)
{
    uint8_t* q = ls + ((mod + var) & LS_MASK);
    memset(q, 0, 16);
    ps_st32(q, v);
}

static void commit(SPUContext* ctx, uint8_t* ls, const mp3_state* d, uint32_t mod, int tag, uint32_t consumed, int xing)
{
    const uint32_t p_consumed = pref(ctx, 4) & LS_MASK, p_pcm = pref(ctx, 5) & LS_MASK, heap = pref(ctx, 6) & LS_MASK;
    const uint32_t old0 = pref(ctx, 7) & LS_MASK, old1 = pref(ctx, 8) & LS_MASK, buffs0 = pref(ctx, 9) & LS_MASK;
    const uint32_t buffs1 = pref(ctx, 10) & LS_MASK, backup = pref(ctx, 11) & LS_MASK, p_bo = pref(ctx, 12) & LS_MASK;
    uint32_t start_dec = 0;
    ps_spu_rdch(ctx, (mod + 0x7448) & PS_SPU_PC_MASK, PS_SPU_RdDec, &start_dec);

    memset(ls + heap, 0, HEAP_SIZE);
    ps_st32(ls + p_pcm, 0);
    ps_st32(ls + p_consumed, 0);
    st_var(ls, mod, VAR_globalSpaceAvailable, HEAP_SIZE);
    st_var(ls, mod, VAR_oldBlockCh0, old0);
    st_var(ls, mod, VAR_oldBlockCh1, old1);
    st_var(ls, mod, VAR_buffs0, buffs0);
    st_var(ls, mod, VAR_buffs1, buffs1);
    st_var(ls, mod, VAR_backup, backup);
    st_var(ls, mod, VAR_globalHeap, heap);
    st_quad1(ls, mod, VAR_startTimer, start_dec);
    init_layer3_vars(ls, mod);
    st_var(ls, mod, VAR_bsspace, heap + HEAP_BSSPACE);
    st_var(ls, mod, VAR_pcm_sample, heap + HEAP_PCM);
    st_var(ls, mod, VAR_pcm_point, 0);
    st_var(ls, mod, VAR_xing_header, (uint32_t)xing);
    ps_st32(ls + p_consumed, consumed);
    uint32_t ret = 0;
    const uint32_t bo_in = ps_ld32(ls + p_bo);
    if (tag) {
        st_var(ls, mod, VAR_globalSpaceTaken, HEAP_PCM + 8320);
        st_var(ls, mod, VAR_bufferLoc, bo_in);
    } else {
        memcpy(ls + heap + HEAP_BSSPACE, d->bs, BS_SIZE);
        for (int i = 0; i < d->pcm_point / 2; i++) {
            ps_st16(ls + heap + HEAP_PCM + 2 * i, (uint16_t)d->pcm[0][i]);
            if (d->fr.stereo == 2) ps_st16(ls + heap + HEAP_PCM + PCM_CH1 + 2 * i, (uint16_t)d->pcm[1][i]);
        }
        /* the granule's spectra (as the hybrid filter left them), its output, and the last new block */
        for (int ch = 0; ch < 2; ch++) {
            st_floats(ls + heap + HEAP_HYBRID_IN + 2304 * (uint32_t)ch, &d->hybridIn[ch][0][0], SBLIMIT * SSLIMIT);
            st_floats(ls + heap + HEAP_HYBRID_OUT + 2304 * (uint32_t)ch, &d->hybridOut[ch][0][0], SBLIMIT * SSLIMIT);
        }
        st_floats(ls + heap + HEAP_HYBRID_TEMP, d->newblock, SBLIMIT * SSLIMIT);
        for (int ch = 0; ch < d->fr.stereo; ch++) {
            st_floats(ls + (ch ? old1 : old0), d->block[ch], SBLIMIT * SSLIMIT);
            st_floats(ls + (ch ? buffs1 : buffs0), &d->buffs[ch][0][0], 2 * 0x110);
        }
        memcpy(ls + backup, d->bs + 512 + d->fr.framesize - 512, 512);
        ps_st32(ls + p_bo, (uint32_t)d->bo);
        ps_st32(ls + p_pcm, xing ? 0xFFFFFFFFu : (uint32_t)d->pcm_point);
        st_var(ls, mod, VAR_pcm_point, (uint32_t)d->pcm_point);
        st_var(ls, mod, VAR_bufferLoc, (uint32_t)d->bo);
        st_var(ls, mod, VAR_bsbuf, heap + HEAP_BSSPACE + 512);
        st_var(ls, mod, VAR_globalSpaceTaken, HEAP_TAKEN_AFTER);
        st_var(ls, mod, VAR_ssize, (uint32_t)d->ssize);
        st_var(ls, mod, VAR_bsi, d->bi);
        st_var(ls, mod, VAR_bsi + 4, heap + HEAP_BSSPACE + d->wp);
        {
            const mp3_frame* fr = &d->fr;
            const uint32_t v[17] = {(uint32_t)fr->stereo, 0, (uint32_t)fr->lsf, (uint32_t)fr->mpeg25, (uint32_t)fr->lay, 0,
                (uint32_t)fr->error_protection, (uint32_t)fr->bitrate_index, (uint32_t)fr->sampling_frequency,
                (uint32_t)fr->padding, (uint32_t)fr->extension, (uint32_t)fr->mode, (uint32_t)fr->mode_ext,
                (uint32_t)fr->copyright, (uint32_t)fr->original, (uint32_t)fr->emphasis, (uint32_t)fr->framesize};
            for (uint32_t i = 0; i < 17; i++)
                if (i != 1 && i != 5) st_var(ls, mod, VAR_fr + 4u * i, v[i]);
        }
        uint32_t end_dec = 0;
        ps_spu_rdch(ctx, (mod + 0x7668) & PS_SPU_PC_MASK, PS_SPU_RdDec, &end_dec);
        st_quad1(ls, mod, VAR_endTimer, end_dec);
        st_var(ls, mod, VAR_procTime, start_dec - end_dec);
        ret = HEAP_PCM;
    }
    /* $3: the PCM's address (the heap's register plus 2304 in every word: the module's pcm_sample), 0 for a tag */
    for (int w = 0; w < 4; w++) ctx->gpr[3].u32[w] = tag ? 0 : ctx->gpr[6].u32[w] + ret;
    ctx->pc = ctx->gpr[0].u32[3] & PS_SPU_PC_MASK;
}

int ps_flashmp3_884a_1e820(SPUContext* ctx, uint8_t* ls)
{
    const uint32_t mod = (MOD_LIFTED_AT + ctx->reloc) & LS_MASK;
    const uint32_t frame = pref(ctx, 3) & LS_MASK, p_consumed = pref(ctx, 4) & LS_MASK, p_pcm = pref(ctx, 5) & LS_MASK;
    const uint32_t heap = pref(ctx, 6) & LS_MASK, old0 = pref(ctx, 7) & LS_MASK, old1 = pref(ctx, 8) & LS_MASK;
    const uint32_t buffs0 = pref(ctx, 9) & LS_MASK, buffs1 = pref(ctx, 10) & LS_MASK, backup = pref(ctx, 11) & LS_MASK;
    const uint32_t p_bo = pref(ctx, 12) & LS_MASK;
    /* Aligned buffers and words inside local store, none on the module (the module copes with others in ways not
     * done here). */
    if ((heap | old0 | old1 | buffs0 | buffs1 | backup) & 15) return 0;
    if ((p_consumed | p_pcm | p_bo) & 3) return 0;
    if (!in_ls(heap, HEAP_SIZE) || !in_ls(old0, 2304) || !in_ls(old1, 2304) || !in_ls(buffs0, 2176) ||
        !in_ls(buffs1, 2176) || !in_ls(backup, 512) || !in_ls(frame, 4 + 4096 + 16))
        return 0;
    if (heap < mod + 0x10380 && heap + HEAP_SIZE > mod) return 0;

    pthread_once(&g_tables_once, init_tables_default_fp);
    mp3_state* d = &t_state;
    d->bad = 0;
    d->rval = ps_ld32(ls + ((mod + VAR_rval) & LS_MASK));
    d->rval_uc = ls[(mod + VAR_rval_uc) & LS_MASK];

    /* readFrame */
    const uint8_t* f = ls + frame;
    int tag = 0;          /* 0: a frame; 1: an ID3/TAG skipped */
    uint32_t consumed = 0;
    int xing = 0;
    if (f[0] == 'I' && f[1] == 'D' && f[2] == '3') {
        tag = 1;
        consumed = f[3] == 1 ? 128 : (uint32_t)f[9] + ((uint32_t)f[8] << 7) + ((uint32_t)f[7] << 14) + ((uint32_t)f[6] << 21) + 10;
    } else if (f[0] == 'T' && f[1] == 'A' && f[2] == 'G') {
        tag = 1;
        consumed = 128;
    }
    if (!tag) {
        const uint32_t h = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | f[3];
        memset(&d->fr, 0, sizeof d->fr);
        if (!decode_header(&d->fr, h, &d->ssize)) return 0; /* the module prints and fails: the lifted code does it */
        const uint8_t* x = f + 4 + (d->fr.stereo == 1 ? 17 : 32);
        if ((x[0] == 'X' && x[1] == 'i' && x[2] == 'n' && x[3] == 'g') || (x[0] == 'I' && x[1] == 'n' && x[2] == 'f' && x[3] == 'o'))
            xing = 1;
        consumed = 4 + (uint32_t)d->fr.framesize;
    }

    if (!tag) {
        /* the bitstream buffer: zeros (the heap is zeroed), the reservoir's bytes where set_pointer puts them, the frame
         * at 512 */
        memset(d->bs, 0, sizeof d->bs);
        memcpy(d->bs + 512, f + 4, (size_t)d->fr.framesize);
        d->wp = 512;
        d->bi = 0;
        if (d->fr.error_protection) getbits(d, 16);
        /* Read the side information first to know main_data_begin, then place the reservoir as set_pointer does. The
         * side information is read again by decode_packet from the same bits (no state changes in between). */
        {
            const uint32_t wp0 = d->wp, bi0 = d->bi, rval0 = d->rval;
            const uint8_t rval_uc0 = d->rval_uc;
            int ms_stereo = d->fr.mode == 1 ? d->fr.mode_ext & 0x2 : 0;
            get_side_info(d, d->fr.stereo, ms_stereo, d->fr.sampling_frequency);
            const unsigned backstep = d->si.main_data_begin;
            d->wp = wp0;
            d->bi = bi0;
            d->rval = rval0;
            d->rval_uc = rval_uc0;
            if (backstep) memcpy(d->bs + 512 + d->ssize - backstep, ls + backup + 512 - backstep, backstep);
        }
        /* the state the module keeps in the caller's buffers */
        for (int ch = 0; ch < 2; ch++) {
            ld_floats(d->block[ch], ls + (ch ? old1 : old0), SBLIMIT * SSLIMIT);
            ld_floats(&d->buffs[ch][0][0], ls + (ch ? buffs1 : buffs0), 2 * 0x110);
        }
        d->bo = (int)ps_ld32(ls + p_bo);
        d->pcm_point = 0;
        memset(d->pcm, 0, sizeof d->pcm);
        if (d->bo < 0 || d->bo > 15) return 0; /* not a position the module wrote */
        decode_packet(d);
        if (d->bad) return 0;
        if (d->wp + 2 >= BS_SIZE) return 0; /* the reads went past the bitstream buffer into the PCM: not modelled */
    }

    /* ---- commit: what the module leaves ---- */
    commit(ctx, ls, d, mod, tag, consumed, xing);
    return 1;
}
