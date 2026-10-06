// cellAdec: the audio decoder games use for their movies' and streams' audio (D-197). The front end keeps Sony's
// context in the memory the game gives (AdecContext, its frames and bitstream information blocks, then the core's
// memory) and decodes through a core: pspocket's own LPCM core (adec_lpcm.cpp) for the three LPCM types; for every
// other codec, the firmware's core module (ATRAC3plus, MP3, AC-3, AAC, ...) through the operation table it exports, as
// Sony's libadec and RPCS3's cellAdec call it. The core modules are the host's imports of cellAdec
// (Registry::add_imports): the loader chooses and links them for a game importing cellAdec, as it would for libadec.
// pspocket's own cores on FFmpeg replace them one codec at a time.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.cpp/.h (get_core_ops, AdecContext, the notifications, the API).

#include "adec.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {
namespace {

// What Sony's libadec imports from the core modules (and so pspocket's cellAdec, D-197): each core's operation table
// and each core's _SceAdecCorrectPtsValue_* (RPCS3 cellAdec.cpp's module declarations; checked against libadec's own
// imports in the user's 4.92 firmware). DTS, DTS-HD core, WMA Pro and WMA Lossless are in no retail firmware.
constexpr HostImport kImports[] = {
    {{}, "cell_libac3dec", 0xC58BB170, true},     // AC-3 operations
    {{}, "cellAtracXdec", 0x4944AF9A, true},      // ATRAC3plus
    {{}, "cellAtracXdec", 0x076B33AB, true},      // ATRAC3plus, 2 channels
    {{}, "cellAtracXdec", 0x1D210EAA, true},      // ATRAC3plus, 6 channels
    {{}, "cellAtracXdec", 0xE9A86E54, true},      // ATRAC3plus, 8 channels
    {{}, "cellMP3dec", 0x84276A23, true},         // MP3
    {{}, "cellAtrac3dec", 0x60487D65, true},      // ATRAC3
    {{}, "cellM2BCdec", 0x300AFE4D, true},        // MPEG-1/2 layer II
    {{}, "cellM2AACdec", 0xAC2F0831, true},       // MPEG-2 AAC
    {{}, "cellDDPdec", 0xF5E9C15C, true},         // E-AC-3
    {{}, "cellTRHDdec", 0xE88E381B, true},        // TrueHD
    {{}, "cellDTSdec", 0x15248EC5, true},         // DTS
    {{}, "cellCelpDec", 0xFFE42C22, true},        // CELP
    {{}, "cellM4AacDec", 0xAB61278D, true},       // MPEG-4 AAC
    {{}, "cellWMAdec", 0x88320B10, true},         // WMA
    {{}, "cellDTSLBRdec", 0x8C50AF52, true},      // DTS Express
    {{}, "cellM4AacDec2ch", 0xE996C664, true},    // MPEG-4 AAC, 2 channels
    {{}, "cellDTSHDdec", 0x51AC2B6C, true},       // DTS-HD
    {{}, "cellMPL1dec", 0xBBC70551, true},        // MPEG-1 layer I
    {{}, "cellMP3Sdec", 0x292CDF0A, true},        // MP3 surround
    {{}, "cellM4AacDec2chmod", 0xDBD26836, true}, // MPEG-4 AAC, 2 channels (mod)
    {{}, "cellCelp8Dec", 0xF0190C6C, true},       // CELP 8 kHz
    {{}, "cellWMAPROdec", 0xA8BAC670, true},      // WMA Pro
    {{}, "cellWMALSLdec", 0x602AAB16, true},      // WMA Lossless
    {{}, "cellDTSHDCOREdec", 0x6C8F4F1C, true},   // DTS-HD core
    {{}, "cellAtrac3multidec", 0xC20C6BD7, true}, // ATRAC3 multichannel
    {{}, "cellDDPdec", 0xEE2731D3, false},         // _SceAdecCorrectPtsValue_Ddp
    {{}, "cellDTSHDdec", 0xF3AF1093, false},       // _SceAdecCorrectPtsValue_DtsHd
    {{}, "cellCelpDec", 0xE4B8FC4D, false},        // _SceAdecCorrectPtsValue_Celp
    {{}, "cellM2AACdec", 0x7CBA9865, false},       // _SceAdecCorrectPtsValue_Aac
    {{}, "cellM2BCdec", 0xEC7DE355, false},        // _SceAdecCorrectPtsValue_mpmc
    {{}, "cellTRHDdec", 0x75080F7B, false},        // _SceAdecCorrectPtsValue_truehd
    {{}, "cellDTSdec", 0x8C7AD5ED, false},         // _SceAdecCorrectPtsValue_DtsCore
    {{}, "cellM4AacDec", 0x431A1DCA, false},       // _SceAdecCorrectPtsValue_M4Aac
    {{}, "cellM4AacDec2ch", 0x869E328D, false},    // _SceAdecCorrectPtsValue_M4Aac2ch
    {{}, "cellM4AacDec2chmod", 0x156FE2F0, false}, // _SceAdecCorrectPtsValue_M4Aac2chmod
    {{}, "cellWMAdec", 0x46E6C806, false},         // _SceAdecCorrectPtsValue_wma
    {{}, "cellDTSLBRdec", 0x185591D6, false},      // _SceAdecCorrectPtsValue_DtsLbr
    {{}, "cellMPL1dec", 0xA4A9A309, false},        // _SceAdecCorrectPtsValue_mpmcl1
    {{}, "cellMP3Sdec", 0x73A2C895, false},        // _SceAdecCorrectPtsValue_Mp3s
    {{}, "cellCelp8Dec", 0x881583C3, false},       // _SceAdecCorrectPtsValue_Celp8
    {{}, "cellWMAPROdec", 0x8D45F21A, false},      // _SceAdecCorrectPtsValue_WmaPro
    {{}, "cellWMALSLdec", 0xF2F9C183, false},      // _SceAdecCorrectPtsValue_WmaLsl
    {{}, "cellDTSHDCOREdec", 0xD4C180D4, false},   // _SceAdecCorrectPtsValue_DtsHd_Core
};

} // namespace

void register_cellAdec(Registry& r)
{
    r.add_imports("cellAdec", kImports);
}

} // namespace pspocket::hle
