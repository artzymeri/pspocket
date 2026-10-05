#pragma once

// cellSsl's state for other HLE modules.

namespace pspocket::hle {

// Whether cellSslInit ran and cellSslEnd hasn't since (RPCS3 _cellSslIsInitd): cellHttpsEnd needs it.
bool ssl_initialized();

} // namespace pspocket::hle
