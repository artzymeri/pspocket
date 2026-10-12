// cellSsl against libssl.sprx (D-193's method, D-530): the same scripts of calls in two fresh processes, one calling
// pspocket's functions (runtime/hle/src/cellSsl*.cpp) and one Sony's module; every answer, every byte written to the
// scripts' memory, libssl's pool (its heap: the same blocks at the same places) and the lv2 calls the script's thread
// makes the same. The console's certificates come from the installed firmware's /dev_flash, as libssl reads them; the
// other inputs are made up here. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/recomp/nid.hpp"
#include "ssl.hpp" // runtime/hle/src: where pspocket keeps libssl's globals

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOut = kData + 0x10000, kOutSize = 0x20000; // the scripts' memory
constexpr u32 kIn = kData + 0x30000, kInSize = 0xC0000;   // inputs: strings, certificates, keys
constexpr u32 kPool = kData + 0x100000, kPoolSize = 0x100000; // cellSslInit's pool
constexpr u8 kFill = 0xA5;

// Script flags.
constexpr u32 kMaskAnswer = 1; // the answer depends on the run
constexpr u32 kSign = 2;       // compared only as negative, 0 or positive
constexpr u32 kStoreStr = 4;   // the answer is a string pointer: its text is copied to `store`, the pointer masked

// An argument read when the call is made: the 32-bit word at the address.
constexpr u64 kDeref = u64{1} << 63;

// A call (its answer also stored at `store` when given). `fn` is a cellSsl function (a name, or "cellSsl_" and the NID);
// "=" stores a 32-bit word (address, value), "in" an input (address, index into inputs()), "pool" keeps the pool's
// bytes for comparing, "~" makes an address the run chose comparable (its offset into the pool, or 1).
struct Op {
    const char* fn;
    std::vector<u64> args;
    u32 flags = 0;
    u32 store = 0;
};

struct Run {
    std::vector<u32> answers;
    std::vector<u8> out;
    std::vector<std::vector<u8>> pools;
    std::vector<std::string> calls;
    std::vector<std::pair<u32, u32>> globals; // Sony's module's segments, or pspocket's block
};

struct Calls {
    std::mutex mutex;
    std::thread::id thread;
    bool on = false; // only while a cellSsl call runs
    std::vector<std::string> list;
};

bool stack_address(u64 v) { return (v >= kStacks && v < kStacks + 0x100000) || (v >= 0xD0000000 && v < 0xF0000000); }

// An lv2 object's ID (they differ between the runs).
bool lv2_id(u32 v)
{
    const u32 top = v >> 24;
    return top == 0x85 || top == 0x86 || top == 0x88 || top == 0x8d || (top >= 0x95 && top <= 0x98);
}

// The arguments compared of each lv2 call.
u32 arg_count(u32 number)
{
    switch (number) {
    case 1: return 0;
    case 30: case 101: case 106: case 141: case 714: return 1;
    case 25: case 100: case 351: case 706: case 712: case 722: return 2;
    case 105: case 700: case 701: case 702: case 703: case 704: case 708: case 709: case 713: case 715: case 721: return 3;
    case 705: case 711: case 716: return 5;
    case 707: case 710: return 6;
    default: return 4;
    }
}

void observe(void* user, u32 number, const u64* a)
{
    auto* c = static_cast<Calls*>(user);
    if (std::this_thread::get_id() != c->thread || !c->on) return;
    std::string line = std::to_string(number);
    for (u32 i = 0; i < arg_count(number); i++) {
        const u32 v = static_cast<u32>(a[i]);
        line += stack_address(a[i]) ? std::string(" stack") : lv2_id(v) ? std::string(" id") : std::format(" {:#x}", a[i]);
    }
    std::lock_guard lock(c->mutex);
    c->list.push_back(line);
}

// ------------------------------------------------------------------------------------------------- inputs

// A file of the installed firmware's /dev_flash (empty when it is not there).
std::string flash_file(const std::string& flash, const std::string& path)
{
    std::ifstream in(flash + path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Made-up certificates and keys (512-bit RSA, made with OpenSSL for these tests): a CA (SHA-1, its own issuer, basic
// constraints, key usage, key identifiers) and a server certificate it signed (MD5, a GeneralizedTime notAfter, a
// subject alternative name, an extended key usage); their keys as PKCS #1 and PKCS #8.
constexpr std::string_view kCaPem = R"(-----BEGIN CERTIFICATE-----
MIIClDCCAj6gAwIBAgIFEjRWeJAwDQYJKoZIhvcNAQEFBQAwgZ0xCzAJBgNVBAYT
AlVTMRMwEQYDVQQIDApUZXN0IFN0YXRlMRIwEAYDVQQHDAlUZXN0IENpdHkxFjAU
BgNVBAoMDXBzcG9ja2V0IHRlc3QxEzARBgNVBAsMCnVuaXQgdGVzdHMxGTAXBgNV
BAMMEHBzcG9ja2V0IHRlc3QgQ0ExHTAbBgkqhkiG9w0BCQEWDmNhQGV4YW1wbGUu
Y29tMB4XDTI2MTAwOTEyMDgxNloXDTM2MTAwNjEyMDgxNlowgZ0xCzAJBgNVBAYT
AlVTMRMwEQYDVQQIDApUZXN0IFN0YXRlMRIwEAYDVQQHDAlUZXN0IENpdHkxFjAU
BgNVBAoMDXBzcG9ja2V0IHRlc3QxEzARBgNVBAsMCnVuaXQgdGVzdHMxGTAXBgNV
BAMMEHBzcG9ja2V0IHRlc3QgQ0ExHTAbBgkqhkiG9w0BCQEWDmNhQGV4YW1wbGUu
Y29tMFwwDQYJKoZIhvcNAQEBBQADSwAwSAJBALydtSrZT/Q/Bw6XPm7xS2MglnVT
z8vam5oRU+fudRWvfq9onX/Xph4v/K010AaSMnW0ok/8o9cLj8WqC164sl8CAwEA
AaNjMGEwHQYDVR0OBBYEFA0IX/iZbmIiNdxoAnpoESYgIU48MB8GA1UdIwQYMBaA
FA0IX/iZbmIiNdxoAnpoESYgIU48MBIGA1UdEwEB/wQIMAYBAf8CAQIwCwYDVR0P
BAQDAgEGMA0GCSqGSIb3DQEBBQUAA0EAlHDRamH4COTwl9E5agJdl1lbSBkbtfxP
aHQmPFRLZJHOpRTqgIzRTp1J90WLweMzHdjHVunQBsK8VEPNEx2NXA==
-----END CERTIFICATE-----
)";
constexpr std::string_view kLeafPem = R"(-----BEGIN CERTIFICATE-----
MIICXDCCAgagAwIBAgIBBzANBgkqhkiG9w0BAQQFADCBnTELMAkGA1UEBhMCVVMx
EzARBgNVBAgMClRlc3QgU3RhdGUxEjAQBgNVBAcMCVRlc3QgQ2l0eTEWMBQGA1UE
CgwNcHNwb2NrZXQgdGVzdDETMBEGA1UECwwKdW5pdCB0ZXN0czEZMBcGA1UEAwwQ
cHNwb2NrZXQgdGVzdCBDQTEdMBsGCSqGSIb3DQEJARYOY2FAZXhhbXBsZS5jb20w
IBcNMjAwMTAyMDMwNDA1WhgPMjA1MTAxMDEwMDAwMDBaMD4xCzAJBgNVBAYTAkpQ
MRUwEwYDVQQKDAxFeGFtcGxlIExlYWYxGDAWBgNVBAMMD3d3dy5leGFtcGxlLmNv
bTBcMA0GCSqGSIb3DQEBAQUAA0sAMEgCQQCl2r9AeXpLDAKuVjIH2tOvlziY7O0r
/n/LV6k24qcSr4SmpkSNWTLn5y2raCFGIEOi6CzvwuF8Pk1ArNbty4otAgMBAAGj
gYwwgYkwJwYDVR0RBCAwHoIPd3d3LmV4YW1wbGUuY29tggtleGFtcGxlLmNvbTAT
BgNVHSUEDDAKBggrBgEFBQcDATAJBgNVHRMEAjAAMB0GA1UdDgQWBBSUKJwrwka6
r6wcctqs3RfcdmkIsjAfBgNVHSMEGDAWgBQNCF/4mW5iIjXcaAJ6aBEmICFOPDAN
BgkqhkiG9w0BAQQFAANBAI0oC0f3yXdc7k6LoODFOnECg3v6oLzzyD5GUAqaMrFs
6oLwTPCg7To9eyDtAFF6BLWFTMs+bth/plZ0ro5vwT4=
-----END CERTIFICATE-----
)";
constexpr std::string_view kCaKey = R"(-----BEGIN RSA PRIVATE KEY-----
MIIBOQIBAAJBALydtSrZT/Q/Bw6XPm7xS2MglnVTz8vam5oRU+fudRWvfq9onX/X
ph4v/K010AaSMnW0ok/8o9cLj8WqC164sl8CAwEAAQJABQH66WRTpxx1R4HkenWV
ltAqrnrHLZZkQzbqj2HZYfX3JlHHB6DqR1cGBU/r3Y6fgC0kugbG5Ep4TVVmDhmA
AQIhAO4p227s4sLWgfPP1zi+0YAMfdfUX3BFcs3xrQrrFkJfAiEAyr3pYPqKrRmV
tKi07ZK3IAy5Tm4mxtUTFD9vQwbTkAECIDGsb0UaiU1D+kTE88m1GvMPJ+wtMl6z
TXeg0Yu2bqUXAiB4bCtGFiruvGamoxqsjlvFZqlew6C2s3McsL0Wk+RwAQIgM/LJ
3pLdx64gt2QlRKdOu0ODjXmbFXUtHmZTWhymzDE=
-----END RSA PRIVATE KEY-----
)";
constexpr std::string_view kLeafKey = R"(-----BEGIN RSA PRIVATE KEY-----
MIIBOQIBAAJBAKXav0B5eksMAq5WMgfa06+XOJjs7Sv+f8tXqTbipxKvhKamRI1Z
MufnLatoIUYgQ6LoLO/C4Xw+TUCs1u3Lii0CAwEAAQJAYCQrlU5Vy8cMjs68db0t
1K2OS2ecHFKM9ey81GoSe0uARw3E0ukfZml4+i9+gyhj2HWhaY6jI3If/H+0CZE6
gQIhANWnU1QcZZtIZ37ZwxTSPnVl7KWIu1cCR22S6EEqff5dAiEAxroeOwh59Ur9
mtIuPf2WGOV3BYqVAbjvWANFAYSt3hECIFblgjfAgPbiQO3P5c8vH6Yh+VjuPrvJ
uoFvcbUnci0lAiAux3METGuA/UUTY7R07TOe6q0IxeVrgV/95M8im1veMQIgbBrR
gFBK4hVKaibOqHDyiGttW1JWrcJvmr+xHEx8lS4=
-----END RSA PRIVATE KEY-----
)";
constexpr std::string_view kLeafKey8 = R"(-----BEGIN PRIVATE KEY-----
MIIBUwIBADANBgkqhkiG9w0BAQEFAASCAT0wggE5AgEAAkEApdq/QHl6SwwCrlYy
B9rTr5c4mOztK/5/y1epNuKnEq+EpqZEjVky5+ctq2ghRiBDougs78LhfD5NQKzW
7cuKLQIDAQABAkBgJCuVTlXLxwyOzrx1vS3UrY5LZ5wcUoz17LzUahJ7S4BHDcTS
6R9maXj6L36DKGPYdaFpjqMjch/8f7QJkTqBAiEA1adTVBxlm0hnftnDFNI+dWXs
pYi7VwJHbZLoQSp9/l0CIQDGuh47CHn1Sv2a0i49/ZYY5XcFipUBuO9YA0UBhK3e
EQIgVuWCN8CA9uJA7c/lzy8fpiH5WO4+u8m6gW9xtSdyLSUCIC7HcwRMa4D9RRNj
tHTtM57qrQjF5WuBX/3kzyKbW94xAiBsGtGAUEriFUpqJs6ocPKIa21bUlatwm+a
v7EcTHyVLg==
-----END PRIVATE KEY-----
)";

// The DER inside a PEM block (base64 between the BEGIN and END lines).
std::string pem_der(std::string_view pem)
{
    const usize begin = pem.find('\n', pem.find("-----BEGIN"));
    const usize end = pem.find("-----END");
    std::string out;
    u32 acc = 0, bits = 0;
    for (usize i = begin; i < end; i++) {
        const char c = pem[i];
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        if (v < 0) continue;
        acc = acc << 6 | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// Inputs the scripts put into memory (by index): strings and byte blocks. The console's certificates come from the
// installed firmware.
std::vector<std::string> inputs(const std::string& flash)
{
    const std::string ca01 = flash_file(flash, "/data/cert/CA01.cer");
    std::vector<std::string> v = {
        "",                       // 0
        ca01,                     // 1 a console certificate (PEM)
        std::string(kCaPem),      // 2
        std::string(kLeafPem),    // 3
        std::string(kCaKey),      // 4
        std::string(kLeafKey),    // 5
        std::string(kLeafKey8),   // 6
        pem_der(kCaPem),          // 7 DER
        pem_der(kLeafPem),        // 8
        pem_der(ca01),            // 9
        pem_der(kLeafKey),        // 10
    };
    return v;
}

// An input's address (0x1000 apart for the small ones; certificates have room).
u64 in(u32 i) { return kIn + 0x2000 * u64{i}; }
// An input's size.
u32 in_size(const std::string& flash, u32 i) { return static_cast<u32>(inputs(flash)[i].size()); }

// ------------------------------------------------------------------------------------------------- tracing

// Tracing Sony's module (PSPOCKET_SSL_TRACE=FILE, with PSPOCKET_SSL_STUBS naming libssl's import stubs by offset):
// every function libssl's code calls, with its depth, every import with its arguments and answer.
struct Tracer {
    u32 lo = 0, hi = 0;
    std::map<u32, std::string> stubs; // module offset -> import
    FILE* out = nullptr;
    int depth = 0;
    std::string step;
};

void trace_executor(void* user, PPUContext& ctx, u8* base, u32 entry)
{
    Tracer& T = *static_cast<Tracer*>(user);
    const u32 stop = static_cast<u32>(ctx.lr) & ~3u;
    const u64 stop_sp = ctx.gpr[1];
    struct Pending {
        std::string name;
        u64 args[4];
        u32 ret = 0;
    };
    std::vector<Pending> pending;
    ctx.cia = entry;
    for (;;) {
        if (ctx.cia == stop && ctx.gpr[1] == stop_sp) return;
        if (const ps_ppu_fn fn = ps_ppu_dispatch_find(ctx.cia)) {
            fn(&ctx, base);
            ctx.cia = static_cast<u32>(ctx.lr) & ~3u;
            if (!pending.empty() && ctx.cia == pending.back().ret) {
                const Pending& p = pending.back();
                std::fprintf(T.out, "%*s@%s(%#llx, %#llx, %#llx, %#llx) = %#llx\n", 2 * T.depth, "", p.name.c_str(),
                    static_cast<unsigned long long>(p.args[0]), static_cast<unsigned long long>(p.args[1]),
                    static_cast<unsigned long long>(p.args[2]), static_cast<unsigned long long>(p.args[3]),
                    static_cast<unsigned long long>(ctx.gpr[3]));
                pending.pop_back();
            }
            continue;
        }
        const u32 pc = ctx.cia;
        const u32 w = ps_read32(base, pc);
        const bool mine = pc >= T.lo && pc < T.hi;
        if (mine) {
            if (const auto it = T.stubs.find(pc - T.lo); it != T.stubs.end())
                pending.push_back({it->second, {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]}, static_cast<u32>(ctx.lr) & ~3u});
        }
        ppu::step(ctx, base);
        if (!mine) continue;
        const u32 op = w >> 26, xo = (w >> 1) & 0x3FF;
        const bool call = (w & 1) && (op == 18 || (op == 19 && (xo == 528 || xo == 16)));
        if (call && static_cast<u32>(ctx.lr) == pc + 4) {
            if (ctx.cia >= T.lo && ctx.cia < T.hi && !T.stubs.contains(ctx.cia - T.lo)) {
                std::fprintf(T.out, "%*s%x(%#llx, %#llx, %#llx, %#llx)\n", 2 * T.depth, "", ctx.cia - T.lo,
                    static_cast<unsigned long long>(ctx.gpr[3]), static_cast<unsigned long long>(ctx.gpr[4]),
                    static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]));
                T.depth++;
            }
        } else if (w == 0x4E800020) { // blr
            if (T.depth > 0) T.depth--;
        }
    }
}

// ------------------------------------------------------------------------------------------------- runs

Run run(const std::string& flash, const std::vector<Op>& ops, bool sony)
{
    Tracer tracer;
    lv2::Config kconfig;
    const char* trace_path = std::getenv("PSPOCKET_SSL_TRACE");
    if (sony && trace_path) {
        kconfig.executor = trace_executor;
        kconfig.executor_user = &tracer;
    }
    Fixture f({}, kconfig);
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    REQUIRE(f.kernel->vfs_mount("/dev_flash", flash, true));
    GuestThread t(f, 0, "ppu");
    loader::LleModules lle(flash);
    REQUIRE(lle.index());
    lle.force("cellSsl");
    std::string why;
    const loader::PrxModule* ssl = lle.load("cellSsl", *f.hle, &why);
    INFO(why);
    REQUIRE(ssl);
    for (const ModuleEntry& s : lle.take_starts()) run_module_entry(t.ctx, s, 0, 0);
    if (sony && trace_path) {
        tracer.lo = ssl->segments[0].first;
        tracer.hi = tracer.lo + static_cast<u32>(ssl->segments[0].second);
        tracer.out = std::fopen(trace_path, "w");
        REQUIRE(tracer.out);
        std::setvbuf(tracer.out, nullptr, _IOLBF, 0);
        if (const char* stubs = std::getenv("PSPOCKET_SSL_STUBS")) {
            std::ifstream sf(stubs);
            std::string line;
            while (std::getline(sf, line)) {
                const usize sp = line.find(' ');
                if (sp != std::string::npos)
                    tracer.stubs[static_cast<u32>(std::stoul(line.substr(0, sp), nullptr, 16))] = line.substr(sp + 1);
            }
        }
    }
    u8* b = f.base();
    std::memset(b + kOut, kFill, kOutSize);
    std::memset(b + kPool, kFill, kPoolSize);
    std::memset(b + kIn, 0, kInSize);
    const std::vector<std::string> ins = inputs(flash);
    Calls calls;
    calls.thread = std::this_thread::get_id();
    f.kernel->set_syscall_observer(&observe, &calls);
    Run r;
    usize step = 0;
    for (const Op& op : ops) {
        step++;
        const std::string_view name = op.fn;
        if (name == "=") {
            ps_write32(b, static_cast<u32>(op.args[0]), static_cast<u32>(op.args[1]));
            r.answers.push_back(0);
            continue;
        }
        if (name == "in") {
            const std::string& text = ins[op.args[1]];
            std::memcpy(b + op.args[0], text.data(), text.size());
            b[op.args[0] + text.size()] = 0;
            r.answers.push_back(0);
            continue;
        }
        if (name == "~") {
            const u32 at = static_cast<u32>(op.args[0]);
            const u32 v = ps_read32(b, at);
            ps_write32(b, at, v >= kPool && v < kPool + kPoolSize ? v - kPool : v != 0);
            r.answers.push_back(0);
            continue;
        }
        if (name == "pool") {
            r.pools.emplace_back(b + kPool, b + kPool + kPoolSize);
            if (const char* dump = std::getenv("PSPOCKET_SSL_POOLDUMP"); dump && sony) {
                // Sony's pool as words, pointers into the module shown as image addresses (segment 0 at 0, segment
                // 1 at its image address), into the pool as P+offset; up to the last word that isn't the fill.
                FILE* df = std::fopen(dump, "a");
                REQUIRE(df);
                u32 end = kPoolSize;
                while (end > 0 && b[kPool + end - 1] == kFill) end--;
                std::fprintf(df, "== pool at step %zu\n", step - 1);
                for (u32 at = 0; at < end; at += 16) {
                    std::string line = std::format("+{:05x}", at);
                    for (u32 k = 0; k < 16; k += 4) {
                        const u32 v = ps_read32(b, kPool + at + k);
                        std::string note;
                        for (usize sg = 0; sg < ssl->segments.size(); sg++) {
                            const auto& [sa, ss] = ssl->segments[sg];
                            if (v >= sa && v < sa + ss) note = std::format("S{}:{:x}", sg, v - sa + (sg ? 0x54e60u : 0u));
                        }
                        if (v >= kPool && v < kPool + kPoolSize) note = std::format("P+{:x}", v - kPool);
                        line += std::format(" {:08x}{}", v, note.empty() ? "" : "(" + note + ")");
                    }
                    std::fprintf(df, "%s\n", line.c_str());
                }
                std::fclose(df);
            }
            r.answers.push_back(0);
            continue;
        }
        std::array<u64, 8> a{};
        for (usize i = 0; i < op.args.size(); i++)
            a[i] = op.args[i] & kDeref ? ps_read32(b, static_cast<u32>(op.args[i])) : op.args[i];
        const std::string fn(name);
        const bool unnamed = fn.size() == 16 && fn.starts_with("cellSsl_");
        const u32 nid = unnamed ? static_cast<u32>(std::stoul(fn.substr(8), nullptr, 16)) : recomp::nid_of(fn);
        u32 r3 = 0;
        if (tracer.out) std::fprintf(tracer.out, "== %zu %s\n", step - 1, fn.c_str());
        if (sony) {
            const auto it = ssl->functions.find({"cellSsl", nid});
            INFO(fn);
            REQUIRE(it != ssl->functions.end());
            calls.on = true;
            r3 = static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            calls.on = false;
        } else {
            const Function* fp = registry().find("cellSsl", nid);
            INFO(fn);
            REQUIRE(fp);
            for (usize i = 0; i < a.size(); i++) t.ctx.gpr[3 + i] = a[i];
            const u64 sp = t.ctx.gpr[1];
            calls.on = true;
            fp->fn(t.ctx, b);
            calls.on = false;
            CHECK(t.ctx.gpr[1] == sp);
            r3 = static_cast<u32>(t.ctx.gpr[3]);
        }
        if (std::getenv("PSPOCKET_SSL_PROBE")) {
            std::fprintf(stderr, "PROBE %zu %s %s(%s) -> %#x\n", step - 1, sony ? "sony" : "ours", fn.c_str(),
                op.args.empty() ? "" : std::format("{:#x}", op.args[0]).c_str(), r3);
        }
        if (op.flags & kStoreStr) {
            // The returned string (both runs point into their own globals): its bytes go to `store` for comparison.
            // A null return (0) stores nothing; the read stops at memory the run never committed.
            char buf[128] = {0};
            buf[0] = r3 ? '\x01' : '\0'; // whether the pointer is null is itself compared
            for (u32 i = 0; r3 && i < sizeof(buf) - 2 && f.kernel->memory().is_committed(r3 + i); i++) {
                const char c = static_cast<char>(ps_read8(b, r3 + i));
                buf[i + 1] = c;
                if (!c) break;
            }
            std::memcpy(b + op.store, buf, sizeof(buf));
            r3 = 0;
        }
        if (op.store && !(op.flags & kStoreStr)) ps_write32(b, op.store, r3);
        if (op.flags & kMaskAnswer) r3 = 0;
        if (op.flags & kSign) r3 = static_cast<s32>(r3) < 0 ? 0xFFFFFFFF : r3 ? 1 : 0;
        r.answers.push_back(r3);
        std::lock_guard lock(calls.mutex);
        calls.list.push_back(std::format("-- {}", fn));
    }
    f.kernel->set_syscall_observer(nullptr, nullptr);
    if (tracer.out) std::fclose(tracer.out);
    r.out.assign(b + kOut, b + kOut + kOutSize);
    r.calls = calls.list;
    if (sony) {
        for (const auto& [at, size] : ssl->segments) r.globals.emplace_back(at, size);
    } else {
        r.globals.emplace_back(ssl::globals_block(), ssl::kGlobalsSize);
    }
    return r;
}

bool within(const std::vector<std::pair<u32, u32>>& ranges, u32 v)
{
    for (const auto& [at, size] : ranges)
        if (v >= at && v < at + size) return true;
    return false;
}

u32 word(const std::vector<u8>& v, usize at)
{
    return u32{v[at]} << 24 | u32{v[at + 1]} << 16 | u32{v[at + 2]} << 8 | v[at + 3];
}

// Where two pools differ first (word by word), what differs by construction set aside: the heap's magic (liblv2 makes
// it from the timebase; with it every chunk's footer), lv2 IDs, and pointers to each library's own globals.
usize pool_diff(const Run& mine, const Run& sonys, usize k)
{
    const std::vector<u8>& x = mine.pools[k];
    const std::vector<u8>& y = sonys.pools[k];
    const u32 magic_x = word(x, 0x30), magic_y = word(y, 0x30);
    for (usize at = 0; at + 4 <= x.size(); at += 4) {
        const u32 a = word(x, at), b = word(y, at);
        if (a == b) continue;
        bool footer = true;
        for (u32 sh = 0; sh < 32; sh += 8) {
            const u32 bx = (a >> sh) & 0xFF, by = (b >> sh) & 0xFF;
            if (bx != by && (bx ^ ((magic_x >> sh) & 0xFF)) != (by ^ ((magic_y >> sh) & 0xFF))) footer = false;
        }
        if (footer) continue;
        if (lv2_id(a) && lv2_id(b) && (a >> 24) == (b >> 24)) continue;
        if (within(mine.globals, a) && within(sonys.globals, b)) continue;
        return at;
    }
    return x.size();
}

void compare(const std::string& flash, const std::vector<Op>& ops, const std::string& what)
{
    const Run sonys = run(flash, ops, true);
    if (std::getenv("PSPOCKET_SSL_PROBE") && std::string_view(std::getenv("PSPOCKET_SSL_PROBE")) != "both") return;
    const Run mine = run(flash, ops, false);
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {}({:#x}) -> ours {:#x}, Sony's {:#x}", what, i, ops[i].fn,
            ops[i].args.empty() ? 0 : ops[i].args[0], mine.answers[i], sonys.answers[i]));
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    usize out_at = 0;
    while (out_at < mine.out.size() && mine.out[out_at] == sonys.out[out_at]) out_at++;
    INFO(std::format("{}: output differs first at +{:#x}", what, out_at));
    CHECK(out_at == mine.out.size());
    REQUIRE(mine.pools.size() == sonys.pools.size());
    for (usize k = 0; k < mine.pools.size(); k++) {
        const usize at = pool_diff(mine, sonys, k);
        INFO(std::format("{}: pool {} differs first at +{:#x} (ours {:#04x}, Sony's {:#04x})", what, k, at,
            at < mine.pools[k].size() ? mine.pools[k][at] : 0, at < sonys.pools[k].size() ? sonys.pools[k][at] : 0));
        if (at != mine.pools[k].size() && std::getenv("PSPOCKET_SSL_DUMP")) {
            for (usize i = (at > 0x40 ? at - 0x40 : 0) & ~usize{15}; i < at + 0x60 && i < mine.pools[k].size(); i += 16) {
                std::string a, b2;
                for (usize j = 0; j < 16; j += 4) {
                    a += std::format("{:08x} ", word(mine.pools[k], i + j));
                    b2 += std::format("{:08x} ", word(sonys.pools[k], i + j));
                }
                std::fprintf(stderr, "pool %zu +%05zx ours %s| Sony's %s\n", k, i, a.c_str(), b2.c_str());
            }
        }
        CHECK(at == mine.pools[k].size());
    }
    const usize n = std::max(mine.calls.size(), sonys.calls.size());
    for (usize i = 0; i < n; i++) {
        const std::string m = i < mine.calls.size() ? mine.calls[i] : "(none)";
        const std::string s = i < sonys.calls.size() ? sonys.calls[i] : "(none)";
        INFO(std::format("{}: lv2 call {}: ours {}, Sony's {}", what, i, m, s));
        CHECK(m == s);
        if (m != s) break;
    }
}

// ------------------------------------------------------------------------------------------------- scripts

std::vector<Op> setup_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    s.push_back({"_cellSslIsInitd", {}});
    s.push_back({"cellSslEnd", {}});
    s.push_back({"cellSslInit", {0, kPoolSize}});
    s.push_back({"cellSslInit", {kPool, 0}});
    s.push_back({"cellSslInit", {kPool, kPoolSize}});
    s.push_back({"cellSslInit", {kPool, kPoolSize}});
    s.push_back({"_cellSslIsInitd", {}});
    s.push_back({"cellSslGetMemoryInfo", {o, o + 4, o + 0x10}});
    s.push_back({"pool", {}});
    s.push_back({"cellSslEnd", {}});
    s.push_back({"pool", {}});
    s.push_back({"cellSslEnd", {}});
    // Without a heap libssl reports what its stack held as the mallinfo (liblv2 writes nothing for heap 0): only the
    // free size is compared.
    s.push_back({"cellSslGetMemoryInfo", {o + 0x40, 0, 0}});
    s.push_back({"cellSsl_4EE83087", {0x1234, 0x5678}});
    // Again, on a pool that held the first heap.
    s.push_back({"cellSslInit", {kPool, kPoolSize}});
    s.push_back({"cellSsl_4EE83087", {0x1234, 0x5678}});
    s.push_back({"cellSslGetMemoryInfo", {0, 0, 0}});
    s.push_back({"cellSslGetMemoryInfo", {o + 0x80, 0, 0}});
    s.push_back({"pool", {}});
    s.push_back({"cellSslEnd", {}});
    s.push_back({"pool", {}});
    // A small pool: the heap is made, the library context isn't.
    s.push_back({"cellSslInit", {kPool, 0x200}});
    s.push_back({"cellSslInit", {kPool, 0x1000}});
    s.push_back({"pool", {}});
    s.push_back({"cellSslEnd", {}});
    return s;
}

// The self-contained functions a game reaches without a certificate or connection: the version and cipher-id
// conversions, the alert strings, the cipher accessors (null, as there is no handshake), the BIO callback accessors
// (null), the title flag and the OID-to-string table.
std::vector<Op> strings_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    // The conversions (they write their result to a pointer).
    for (u32 v : {0x300u, 0x301u, 0x302u, 0u}) s.push_back({"_cellSslConvertSslVersion", {v, o + 0x10}, 0, static_cast<u32>(o + 0x10)});
    for (u32 v : {3u, 4u, 5u, 6u, 8u, 10u, 0x62u, 100u, 0u, 7u, 9u, 200u})
        s.push_back({"_cellSslConvertCipherId", {v, o + 0x14}, 0, static_cast<u32>(o + 0x14)});
    // The alert strings (the answer is a pointer; its text is compared).
    u32 at = o + 0x100;
    for (u32 v : {0x100u, 0x200u, 0x300u}) { s.push_back({"SSL_alert_type_string_long", {v}, kStoreStr, at}); at += 0x40; }
    for (u32 v : {0u, 10u, 0x14u, 0x1Eu, 0x28u, 0x29u, 0x2Au, 0x2Bu, 0x2Cu, 0x2Du, 0x2Eu, 0x2Fu, 0x33u})
        { s.push_back({"SSL_alert_desc_string_long", {v}, kStoreStr, at}); at += 0x40; }
    // The cipher accessors with no cipher (0): the defaults.
    s.push_back({"SSL_CIPHER_get_id", {0}});
    s.push_back({"SSL_CIPHER_get_name", {0}, kStoreStr, at}); at += 0x40;
    s.push_back({"SSL_CIPHER_get_version", {0}, kStoreStr, at}); at += 0x40;
    s.push_back({"SSL_CIPHER_get_bits", {0, o + 0x18}, 0, static_cast<u32>(o + 0x18)});
    // The BIO callback accessors with no BIO (0).
    s.push_back({"BIO_get_cb_arg", {0}});
    s.push_back({"BIO_set_cb_arg", {0, 0x1234}});
    // The title flag and the OID table need libssl initialised.
    s.push_back({"cellSslInit", {kPool, kPoolSize}});
    s.push_back({"cellSsl_CC6A0CB5", {o + 0x200}, 0, static_cast<u32>(o + 0x208)});
    s.push_back({"cellSsl_404136C1", {0}});
    // SSLCERT_OID_to_string on the standard attribute OIDs {2,5,4,n}, short and long names.
    for (u32 n : {3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 13u, 0x2Au, 0x2Bu, 0x2Du, 99u}) {
        s.push_back({"=", {o + 0x300, 0x55040000u | (n << 8)}});
        s.push_back({"SSLCERT_OID_to_string", {o + 0x300, 3, 0}, kStoreStr, at}); at += 0x40;
        s.push_back({"SSLCERT_OID_to_string", {o + 0x300, 3, 1}, kStoreStr, at}); at += 0x40;
    }
    s.push_back({"cellSslEnd", {}});
    return s;
}

// Discovery (PSPOCKET_SSL_PROBE, Sony's side traced): the certificate functions on the made-up certificates.
std::vector<Op> probe_script(const std::string& flash)
{
    std::vector<Op> s;
    const u64 o = kOut;
    s.push_back({"cellSslInit", {kPool, kPoolSize}});
    for (u32 i = 1; i <= 10; i++) s.push_back({"in", {in(i), i}});
    const u64 cert = o + 0x200, cert2 = o + 0x204;
    s.push_back({"=", {o + 0x100, in(7)}});
    s.push_back({"SSLCERT_from_binary", {0, o + 0x100, in_size(flash, 7)}, 0, static_cast<u32>(cert)});
    s.push_back({"pool", {}});
    s.push_back({"=", {o + 0x104, in(8)}});
    s.push_back({"SSLCERT_from_binary", {0, o + 0x104, in_size(flash, 8)}, 0, static_cast<u32>(cert2)});
    s.push_back({"SSLCERT_get_subject_name", {cert | kDeref}, 0, static_cast<u32>(o + 0x208)});
    s.push_back({"SSLCERT_get_issuer_name", {cert2 | kDeref}, 0, static_cast<u32>(o + 0x20C)});
    s.push_back({"SSLCERT_NAME_get_entry_count", {(o + 0x208) | kDeref}});
    s.push_back({"SSLCERT_NAME_get_entry", {(o + 0x208) | kDeref, 1}, 0, static_cast<u32>(o + 0x210)});
    s.push_back({"SSLCERT_NAME_ENTRY_get_info", {(o + 0x210) | kDeref, o + 0x300, o + 0x310}});
    s.push_back({"SSLCERT_NAME_oneline", {(o + 0x208) | kDeref, o + 0x400, 0x100}});
    s.push_back({"SSLCERT_NAME_oneline", {(o + 0x208) | kDeref, 0, 0}});
    s.push_back({"SSLCERT_NAME_cmp", {(o + 0x208) | kDeref, (o + 0x20C) | kDeref}});
    s.push_back({"SSLCERT_get_notBefore", {cert | kDeref, o + 0x320, o + 0x324, o + 0x328}});
    s.push_back({"SSLCERT_get_notAfter", {cert2 | kDeref, o + 0x330, o + 0x334, o + 0x338}});
    s.push_back({"SSLCERT_get_pubkey", {cert | kDeref}, 0, static_cast<u32>(o + 0x214)});
    s.push_back({"cellSsl_62236E88", {(o + 0x208) | kDeref}});
    s.push_back({"cellSslCertGetSerialNumber", {cert | kDeref, o + 0x340, o + 0x344}});
    s.push_back({"cellSslCertGetPublicKey", {cert | kDeref, o + 0x348, o + 0x34C}});
    s.push_back({"cellSslCertGetRsaPublicKeyModulus", {cert | kDeref, o + 0x350, o + 0x354}});
    s.push_back({"cellSslCertGetRsaPublicKeyExponent", {cert | kDeref, o + 0x358, o + 0x35C}});
    s.push_back({"cellSslCertGetMd5Fingerprint", {cert | kDeref, o + 0x360, o + 0x378}});
    s.push_back({"cellSslCertGetNotBefore", {cert | kDeref, o + 0x380}});
    s.push_back({"cellSslCertGetNotAfter", {cert2 | kDeref, o + 0x388}});
    s.push_back({"cellSslCertGetSubjectName", {cert | kDeref, o + 0x390}});
    s.push_back({"cellSslCertGetIssuerName", {cert2 | kDeref, o + 0x394}});
    s.push_back({"cellSslCertGetNameEntryCount", {(o + 0x208) | kDeref, o + 0x398}});
    s.push_back({"cellSslCertGetNameEntryInfo", {(o + 0x208) | kDeref, 0, o + 0x3A0, o + 0x3A4, o + 0x3A8, 0}});
    s.push_back({"cellSsl_2E8CC56D", {cert | kDeref}});
    s.push_back({"SSLCERT_verify", {cert2 | kDeref, (o + 0x214) | kDeref}});
    s.push_back({"SSLCERT_get_basic_constraints_int", {cert | kDeref, 0, o + 0x3B0, o + 0x3B4}});
    s.push_back({"cellSsl_C132E990", {cert | kDeref}});
    s.push_back({"pool", {}});
    s.push_back({"SSLCERT_free", {cert | kDeref}});
    s.push_back({"SSLCERT_free", {cert2 | kDeref}});
    s.push_back({"pool", {}});
    s.push_back({"cellSslEnd", {}});
    return s;
}

} // namespace

TEST_CASE("cellSsl against libssl: set-up", "[hle][lle][ssl]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, setup_script(), "set-up");
}

TEST_CASE("cellSsl against libssl: conversions and names", "[hle][lle][ssl]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, strings_script(), "strings");
}

TEST_CASE("cellSsl against libssl: probe", "[.][hle][lle][ssl]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, probe_script(*flash), "probe");
}
