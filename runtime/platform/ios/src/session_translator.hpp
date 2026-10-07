#pragma once

// SPU programs translated while the game runs on the phone (D-236): prepare.mm makes the translator (the signing
// identity, the compiler folder, the phone's limits), game.mm starts it once the game library is loaded.

#include <memory>
#include <string>

namespace pspocket {
class GuestMemory;
namespace prepare {
class SessionTranslator;
}
} // namespace pspocket

namespace pspocket::ios {

// The SPU images the game saves into images_dir (its coverage folder) translated in the background and loaded into
// this process. Null (and logged) when this build has no compiler, the phone no signing identity, a preparation is
// running, or PSPOCKET_SPU_TRANSLATE=0.
std::shared_ptr<prepare::SessionTranslator> start_session_translator(const std::string& title_id,
    const std::string& images_dir, GuestMemory& memory);

} // namespace pspocket::ios
