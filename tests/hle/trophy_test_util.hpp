#pragma once

// Synthetic trophy sets for the sceNpTrophy tests (no game data): a TROPHY.TRP in RPCS3's layout (rpcs3/Loader/TRP.h)
// around configuration files the tests write.

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::hle::test {

using TrpFile = std::pair<std::string, std::string>; // name, contents

// A TROPHY.TRP: the 0x40-byte header (magic 0xDCA24D00, version, file size, file count, element size 0x40), the
// 0x40-byte entries (name, offset, size), then the files, 16-byte aligned. From version 2 on, the header holds the
// SHA-1 of the whole file with that field zeroed.
inline std::vector<u8> make_trp(const std::vector<TrpFile>& files, u32 version = 3)
{
    std::vector<u8> out(0x40 + 0x40 * files.size(), 0);
    for (usize i = 0; i < files.size(); i++) {
        while (out.size() % 16) out.push_back(0);
        const u64 offset = out.size();
        out.insert(out.end(), files[i].second.begin(), files[i].second.end());
        u8* e = out.data() + 0x40 + 0x40 * i;
        std::memcpy(e, files[i].first.data(), std::min<usize>(files[i].first.size(), 31));
        ps_st64(e + 32, offset);
        ps_st64(e + 40, files[i].second.size());
    }
    u8* h = out.data();
    ps_st32(h + 0, 0xDCA24D00);
    ps_st32(h + 4, version);
    ps_st64(h + 8, out.size());
    ps_st32(h + 16, static_cast<u32>(files.size()));
    ps_st32(h + 20, 0x40);
    if (version >= 2) {
        const recomp::Sha1Digest sha1 = recomp::Sha1::hash(out);
        std::memcpy(out.data() + 0x1C, sha1.data(), sha1.size());
    }
    return out;
}

// The set's English configuration (TROP_01.SFM; installed as TROPCONF.SFM): a platinum trophy (0) that the bronze (1)
// and the hidden gold (2) lead to, and a silver one (3) outside it. It starts like Sony's files: the signature comment
// before the XML declaration.
inline constexpr std::string_view kTropEnglish =
    "<!--Sce-Np-Trophy-Signature: 0123456789abcdef0123456789abcdef-->\r\n"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<!--This file was written by the pspocket tests.-->\r\n"
    "<trophyconf version=\"1.0\" policy=\"large\">\r\n"
    "<npcommid>NPWR12345_00</npcommid>\r\n"
    "<trophyset-version>01.00</trophyset-version>\r\n"
    "<parental-level license-area=\"default\">0</parental-level>\r\n"
    "<title-name>Synthetic &amp; Test</title-name>\r\n"
    "<title-detail>A trophy set the test suite built</title-detail>\r\n"
    "<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>All of them</name>"
    "<detail>Unlock every other trophy</detail></trophy>\r\n"
    "<trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>First</name>"
    "<detail>The first one</detail></trophy>\r\n"
    "<trophy id=\"002\" hidden=\"yes\" ttype=\"G\" pid=\"000\"><name>Secret</name><detail>A hidden one</detail>"
    "</trophy>\r\n"
    "<trophy id=\"003\" hidden=\"no\" ttype=\"S\" pid=\"-1\"><name><![CDATA[Extra <bonus>]]></name>"
    "<detail>Not for the platinum</detail></trophy>\r\n"
    "</trophyconf>\r\n";

// The same set in the default language (TROP.SFM) and in another one (TROP_02.SFM), and the pack's own TROPCONF.SFM:
// what the English file replaces.
inline constexpr std::string_view kTropDefault =
    "<trophyconf><title-name>Default language</title-name>"
    "<trophy id=\"000\" ttype=\"P\" pid=\"-1\"><name>Default</name></trophy></trophyconf>";
inline constexpr std::string_view kTropFrench =
    "<trophyconf><title-name>Langue 2</title-name>"
    "<trophy id=\"000\" ttype=\"P\" pid=\"-1\"><name>Tous</name></trophy></trophyconf>";
inline constexpr std::string_view kTropConf =
    "<trophyconf><title-name>Configuration</title-name>"
    "<trophy id=\"000\" ttype=\"P\" pid=\"-1\"/></trophyconf>";

// The full pack of the set: every configuration and an icon per trophy.
inline std::vector<TrpFile> sample_trophy_files()
{
    return {
        {"TROPCONF.SFM", std::string(kTropConf)},
        {"TROP.SFM", std::string(kTropDefault)},
        {"TROP_01.SFM", std::string(kTropEnglish)},
        {"TROP_02.SFM", std::string(kTropFrench)},
        {"ICON0.PNG", "\x89PNG game icon"},
        {"TROP000.PNG", "\x89PNG platinum"},
        {"TROP001.PNG", "\x89PNG bronze"},
        {"TROP002.PNG", "\x89PNG gold"},
        {"TROP003.PNG", "\x89PNG silver"},
    };
}

} // namespace pspocket::hle::test
