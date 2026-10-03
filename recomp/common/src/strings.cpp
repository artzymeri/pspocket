#include "pspocket/recomp/strings.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string>

namespace pspocket::recomp {

namespace {

using O = MiddlewareOrigin;

// Signatures: identifiers, module names and copyright or error strings each library leaves behind. Short patterns
// are case-sensitive to keep false positives down; product names stay stable because reports group by them.
constexpr MiddlewareSignature kSignatures[] = {
    // ---- Sony ----
    {"SPURS", "Sony", O::Sony, "cellSpurs", false, false},
    {"SPURS", "Sony", O::Sony, "SPURS", false, false},
    {"libsre (SPU runtime)", "Sony", O::Sony, "libsre", true, false},
    {"Edge zlib", "Sony", O::Sony, "edgezlib", true, true},
    {"Edge zlib", "Sony", O::Sony, "edge_zlib", true, true},
    {"Edge geometry", "Sony", O::Sony, "edgegeom", true, true},
    {"Edge geometry", "Sony", O::Sony, "edge_geom", true, true},
    {"Edge animation", "Sony", O::Sony, "edgeanim", true, true},
    {"Edge animation", "Sony", O::Sony, "edge_anim", true, true},
    {"Edge post-processing", "Sony", O::Sony, "edgepost", true, true},
    {"Edge post-processing", "Sony", O::Sony, "edge_post", true, true},
    {"MLAA", "Sony", O::Sony, "edgemlaa", true, true},
    {"MLAA", "Sony", O::Sony, "edge_mlaa", true, true},
    {"MLAA", "Sony", O::Sony, "MLAA", false, true},
    {"ATRAC3plus", "Sony", O::Sony, "atrac3plus", true, true},
    {"ATRAC3plus", "Sony", O::Sony, "cellAtrac", false, true},
    {"MP3 decoder", "Sony", O::Sony, "mp3dec", true, true},
    {"MP3 decoder", "Sony", O::Sony, "cellmp3", true, true},
    {"libfont", "Sony", O::Sony, "cellFont", false, false},
    {"libfont", "Sony", O::Sony, "libfont", true, false},
    {"PhyreEngine", "Sony", O::Sony, "phyreengine", true, true},
    {"PhyreEngine", "Sony", O::Sony, "Phyre::", false, true},
    {"NP trophies", "Sony", O::Sony, "sceNpTrophy", false, false},
    {"NP trophies", "Sony", O::Sony, "TROPCONF", false, false},
    {"NP (PSN)", "Sony", O::Sony, "sceNp", false, false},
    {"Cell sync libraries", "Sony", O::Sony, "cellSync", false, false},
    {"Cell sync libraries", "Sony", O::Sony, "cellDaisy", false, false},
    {"SCE copyright notice", "Sony", O::Sony, "Sony Computer Entertainment", false, false},

    // ---- third party ----
    {"RAGE", "Rockstar Games", O::ThirdParty, "rage::", false, false},
    {"Euphoria", "NaturalMotion", O::ThirdParty, "NaturalMotion", false, true},
    {"Euphoria", "NaturalMotion", O::ThirdParty, "euphoria", true, true},
    {"Euphoria", "NaturalMotion", O::ThirdParty, "NmRs", false, true},
    {"Havok", "Havok", O::ThirdParty, "havok", true, true},
    {"Havok", "Havok", O::ThirdParty, "hkpWorld", false, true},
    {"Havok", "Havok", O::ThirdParty, "hkBaseSystem", false, true},
    {"Scaleform GFx", "Scaleform", O::ThirdParty, "scaleform", true, true},
    {"Scaleform GFx", "Scaleform", O::ThirdParty, "GFxMovie", false, true},
    {"Bink", "RAD Game Tools", O::ThirdParty, "BinkOpen", false, true},
    {"Bink", "RAD Game Tools", O::ThirdParty, "RAD Game Tools", false, true},
    {"Bink", "RAD Game Tools", O::ThirdParty, "bink video", true, true},
    {"FMOD", "Firelight Technologies", O::ThirdParty, "FMOD", false, true},
    {"FMOD", "Firelight Technologies", O::ThirdParty, "Firelight Technologies", false, true},
    {"Wwise", "Audiokinetic", O::ThirdParty, "wwise", true, true},
    {"Wwise", "Audiokinetic", O::ThirdParty, "AkSoundEngine", false, true},
    {"Wwise", "Audiokinetic", O::ThirdParty, "Audiokinetic", false, true},
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, " inflate 1.", false, true}, // inflate_copyright banner
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, " deflate 1.", false, true}, // deflate_copyright banner
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, "Mark Adler", false, true},
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, "Jean-loup Gailly", false, true},
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, "incorrect header check", false, true},
    {"zlib", "Jean-loup Gailly, Mark Adler", O::ThirdParty, "invalid distance too far back", false, true},
    {"Lua", "Lua.org", O::ThirdParty, "$Lua", false, true},
    {"Lua", "Lua.org", O::ThirdParty, "Lua.org", false, true},
    {"Lua", "Lua.org", O::ThirdParty, "attempt to index", false, true},
    {"Bullet", "Bullet Physics", O::ThirdParty, "btCollisionWorld", false, true},
    {"Bullet", "Bullet Physics", O::ThirdParty, "btRigidBody", false, true},
    {"Bullet", "Bullet Physics", O::ThirdParty, "btDiscreteDynamicsWorld", false, true},
    {"Bullet", "Bullet Physics", O::ThirdParty, "SpuGatheringCollision", false, true},
    {"Bullet", "Bullet Physics", O::ThirdParty, "bullet physics", true, true},
};

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool printable(u8 c) { return (c >= 0x20 && c <= 0x7E) || c == '\t'; }

} // namespace

std::vector<FoundString> extract_strings(std::span<const u8> data, usize min_length, u64 base_offset, usize max_count)
{
    std::vector<FoundString> out;
    if (min_length == 0) min_length = 1;
    usize i = 0;
    while (i < data.size() && out.size() < max_count) {
        if (!printable(data[i])) {
            i++;
            continue;
        }
        const usize start = i;
        while (i < data.size() && printable(data[i])) i++;
        if (i - start >= min_length) {
            out.push_back({base_offset + start, std::string(reinterpret_cast<const char*>(data.data() + start), i - start)});
        }
    }
    return out;
}

std::string_view middleware_origin_name(MiddlewareOrigin origin)
{
    return origin == MiddlewareOrigin::Sony ? "sony" : "third_party";
}

std::span<const MiddlewareSignature> middleware_signatures() { return kSignatures; }

std::vector<MiddlewareHit> detect_middleware(std::span<const u8> data, u64 base_offset)
{
    std::vector<MiddlewareHit> hits;
    if (data.empty()) return hits;

    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    std::string lowered; // built on first use
    for (const MiddlewareSignature& sig : kSignatures) {
        std::string_view haystack = text;
        std::string pattern(sig.pattern);
        if (sig.ignore_case) {
            if (lowered.empty()) {
                lowered.resize(text.size());
                std::transform(text.begin(), text.end(), lowered.begin(), lower);
            }
            haystack = lowered;
            std::transform(pattern.begin(), pattern.end(), pattern.begin(), lower);
        }
        const std::boyer_moore_horspool_searcher searcher(pattern.begin(), pattern.end());
        u32 count = 0;
        u64 first = 0;
        auto it = haystack.begin();
        while (true) {
            const auto found = std::search(it, haystack.end(), searcher);
            if (found == haystack.end()) break;
            if (count == 0) first = static_cast<u64>(found - haystack.begin());
            count++;
            it = found + static_cast<std::ptrdiff_t>(pattern.size());
        }
        if (count == 0) continue;

        auto hit = std::find_if(hits.begin(), hits.end(), [&](const MiddlewareHit& h) { return h.product == sig.product; });
        if (hit == hits.end()) {
            hits.push_back({sig.product, sig.vendor, sig.origin, 0, base_offset + first, sig.identifies_program, {}});
            hit = hits.end() - 1;
        }
        hit->count += count;
        hit->first_offset = std::min(hit->first_offset, base_offset + first);
        hit->identifies_program = hit->identifies_program || sig.identifies_program;
        hit->patterns.push_back(sig.pattern);
    }
    return hits;
}

std::string_view spu_program_class_name(SpuProgramClass c)
{
    switch (c) {
    case SpuProgramClass::SonyMiddleware: return "sony_middleware";
    case SpuProgramClass::ThirdPartyMiddleware: return "third_party_middleware";
    case SpuProgramClass::Custom: return "custom";
    }
    return "custom";
}

SpuProgramClass classify_spu_program(const std::vector<MiddlewareHit>& hits)
{
    bool third_party = false;
    for (const auto& h : hits) {
        if (!h.identifies_program) continue;
        if (h.origin == MiddlewareOrigin::Sony) return SpuProgramClass::SonyMiddleware;
        third_party = true;
    }
    return third_party ? SpuProgramClass::ThirdPartyMiddleware : SpuProgramClass::Custom;
}

} // namespace pspocket::recomp
