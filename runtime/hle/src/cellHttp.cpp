// cellHttp: the HTTP(S) library's setup and its cookie store. Online play is a non-goal (D-024): the library only has
// to come up; nothing here opens a connection. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellHttp.cpp/.h (its
// http_manager state and checks). RPCS3 runs Sony's libhttp by default (g_prx_list: libhttp.sprx is not HLE); its HLE
// functions are the reference here. The memory pools the game hands over are libhttp's own and are not touched.

#include <mutex>
#include <string_view>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "ssl.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellHttp";

enum : u32 {
    CELL_HTTP_ERROR_ALREADY_INITIALIZED = 0x80710001,
    CELL_HTTP_ERROR_NOT_INITIALIZED = 0x80710002,
    CELL_HTTP_ERROR_NO_BUFFER = 0x80710004,
    CELL_SSL_ERROR_NOT_INITIALIZED = 0x80740001,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// RPCS3 http_manager.
struct HttpState {
    std::mutex m;
    bool initialized = false;
    bool cookie_initialized = false;
    bool https_initialized = false;
};

HttpState& http() { return HleProcess::current()->state<HttpState>(); }

s32 done(std::string_view call, s32 r)
{
    PS_LOG_INFO(kChan, "{}: {:#x}", call, static_cast<u32>(r));
    return r;
}

// RPCS3: initialised first, then a pool and a size.
s32 cellHttpInit(PPUContext&, u32 pool, u32 pool_size)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    if (h.initialized) return done("cellHttpInit", error(CELL_HTTP_ERROR_ALREADY_INITIALIZED));
    if (!pool || !pool_size) return done("cellHttpInit", error(CELL_HTTP_ERROR_NO_BUFFER));
    h.initialized = true;
    return done("cellHttpInit", lv2::CELL_OK);
}

// RPCS3: the HTTPS and cookie states stay as they are.
s32 cellHttpEnd(PPUContext&)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    if (!h.initialized) return done("cellHttpEnd", error(CELL_HTTP_ERROR_NOT_INITIALIZED));
    h.initialized = false;
    return done("cellHttpEnd", lv2::CELL_OK);
}

// cellHttpsInit(u32 caCertNum, const CellHttpsData* caList). RPCS3: a TODO with only the initialised check (neither
// cellHttpInit nor the certificate list is required).
s32 cellHttpsInit(PPUContext&, u32 ca_cert_num, u32 ca_list)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    PS_LOG_INFO(kChan, "cellHttpsInit({} CA certificates at {:#x})", ca_cert_num, ca_list);
    if (h.https_initialized) return done("cellHttpsInit", error(CELL_HTTP_ERROR_ALREADY_INITIALIZED));
    h.https_initialized = true;
    return done("cellHttpsInit", lv2::CELL_OK);
}

// RPCS3: HTTPS initialised, then SSL still initialised (_cellSslIsInitd).
s32 cellHttpsEnd(PPUContext&)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    if (!h.https_initialized) return done("cellHttpsEnd", error(CELL_HTTP_ERROR_NOT_INITIALIZED));
    if (!ssl_initialized()) return done("cellHttpsEnd", error(CELL_SSL_ERROR_NOT_INITIALIZED));
    h.https_initialized = false;
    return done("cellHttpsEnd", lv2::CELL_OK);
}

// RPCS3: a pool and a size first, then initialised.
s32 cellHttpInitCookie(PPUContext&, u32 pool, u32 pool_size)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    if (!pool || !pool_size) return done("cellHttpInitCookie", error(CELL_HTTP_ERROR_NO_BUFFER));
    if (h.cookie_initialized) return done("cellHttpInitCookie", error(CELL_HTTP_ERROR_ALREADY_INITIALIZED));
    h.cookie_initialized = true;
    return done("cellHttpInitCookie", lv2::CELL_OK);
}

s32 cellHttpEndCookie(PPUContext&)
{
    HttpState& h = http();
    std::lock_guard lock(h.m);
    if (!h.cookie_initialized) return done("cellHttpEndCookie", error(CELL_HTTP_ERROR_NOT_INITIALIZED));
    h.cookie_initialized = false;
    return done("cellHttpEndCookie", lv2::CELL_OK);
}

} // namespace

void register_cellHttp(Registry& r)
{
    r.add("cellHttp", {
        PS_HLE_FUNC(0x250C386C, cellHttpInit),
        PS_HLE_FUNC(0xD276FF1F, cellHttpEnd),
        PS_HLE_FUNC(0x522180BC, cellHttpsInit),
        PS_HLE_FUNC(0xE6D4202F, cellHttpsEnd),
        PS_HLE_FUNC(0x9638F766, cellHttpInitCookie),
        PS_HLE_FUNC(0x61B2BADE, cellHttpEndCookie),
    });
}

} // namespace pspocket::hle
