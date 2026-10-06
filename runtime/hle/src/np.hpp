#pragma once

// NP's state for the other NP modules, kept by sceNp.cpp (implemented there; the user is never signed in: D-059).
// sceNp2Init and sceNp2Term (sceNp2.cpp) start and stop NP through RPCS3's sceNpInit and sceNpTerm; modules on top of
// NP (sceNpCommerce2) ask whether it is up. Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNp.cpp (sceNpInit, sceNpTerm)
// and rpcs3/Emu/NP/np_handler.cpp (is_NP_init, init_NP, terminate_NP).

#include <optional>
#include <string>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

// Whether NP is initialized (RPCS3 np_handler::is_NP_init): from sceNp2Init until sceNp2Term. Needs a running HLE
// process.
bool np_initialized();

// RPCS3 sceNpInit: CELL_OK, or SCE_NP_ERROR_ALREADY_INITIALIZED, SCE_NP_ERROR_INVALID_ARGUMENT (no pool) or
// SCE_NP_ERROR_INSUFFICIENT_BUFFER (a pool under 128 KB). The pool would hold NP's working memory; nothing uses it
// offline.
u32 np_init(u32 poolsize, u32 poolptr);

// The size of the pool NP was initialized with (RPCS3 np_memory: set up by init, released by term), 0 while NP is down.
// Nothing is allocated from it offline.
u32 np_pool_size();

// RPCS3 sceNpTerm: CELL_OK, or SCE_NP_ERROR_NOT_INITIALIZED. Drops the manager callback, the basic handler and the
// signaling contexts (np_handler::terminate_NP, idm::clear<signaling_ctx>); the lookup library and its title contexts
// stay ("TODO: Other contexts"). terminate_NP also drops the matching 2 room callbacks, which sceNp2.cpp keeps: its
// caller drops them when this succeeds.
u32 np_term();

// What NP's term drops in sceNp2.cpp: the matching 2 room callbacks (np_handler::terminate_NP). sceNpTerm calls it
// after np_term succeeds (sceNp2Term drops them itself).
void np2_on_np_term();

// Whether matching 2 is initialized (RPCS3 np_handler::is_NP2_Match2_init): from sceNpMatching2Init2 until
// sceNpMatching2Term2 or sceNp2Term. The older matching library refuses its calls meanwhile
// (SCE_NP_MATCHING_ERROR_UTILITY_UNAVAILABLE).
bool np_match2_initialized();

// The SceNpCommunicationId at `addr` (12 bytes of guest memory) as RPCS3 names it ("NPWR01234_00") when it is valid
// (np::validate_communication_id: nine characters of 0-9 and A-Z, num at most 99); nullopt otherwise. For the title
// contexts of the community libraries (lookup, score, TUS), whose constructors ensure() it in RPCS3.
std::optional<std::string> np_communication_id(u8* base, u32 addr);

} // namespace pspocket::hle
