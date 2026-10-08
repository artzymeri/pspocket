// cellHttpUtil (runtime/hle/src/cellHttpUtil.cpp, D-5NN): known answers without the firmware, then every function
// against libhttp.sprx, Sony's module of the library, from the user's installed system software (D-193's method): the
// same calls with made-up arguments go to both, and the answers and every byte of the work area (inputs, structures,
// pools, outputs and their guards) must be the same. The comparison is skipped without the firmware
// (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

// The work area: inputs, a structure, a pool, the required/parsed-length words, source structures and their strings,
// and an output buffer, each followed by bytes nothing should touch.
constexpr u32 kArea = kData + 0x1000, kAreaSize = 0x4000;
constexpr u32 kIn = kArea;                 // the input string (up to 0x1000)
constexpr u32 kStruct = kArea + 0x1000;    // the structure filled
constexpr u32 kPool = kArea + 0x1100;      // the pool (up to 0xE00)
constexpr u32 kWords = kArea + 0x2000;     // required at +0, parsed length at +0x10
constexpr u32 kSrc = kArea + 0x2100;       // a source structure
constexpr u32 kSrcText = kArea + 0x2200;   // its strings
constexpr u32 kOut = kArea + 0x3000;       // an output buffer (up to 0x1000)
constexpr u32 kRequired = kWords, kParsed = kWords + 0x10;
constexpr u8 kFill = 0xCC;

constexpr u32 OK = 0;
constexpr u32 NO_MEMORY = 0x80711001, NO_BUFFER = 0x80711002, NO_STRING = 0x80711003, INSUFFICIENT = 0x80711004,
              INVALID_URI = 0x80711005, INVALID_HEADER = 0x80711006, INVALID_REQUEST = 0x80711007,
              INVALID_RESPONSE = 0x80711008, INVALID_LENGTH = 0x80711009, INVALID_CHARACTER = 0x8071100A;

void reset(u8* b) { std::memset(b + kArea, kFill, kAreaSize); }

void put(u8* b, u32 addr, std::string_view s, bool nul = true)
{
    std::memcpy(b + addr, s.data(), s.size());
    if (nul) b[addr + s.size()] = 0;
}

std::string cstr(u8* b, u32 addr)
{
    std::string s;
    while (b[addr + s.size()]) s += static_cast<char>(b[addr + s.size()]);
    return s;
}

// A call: the function's name, its arguments, and what to write into the reset work area first.
struct Call {
    const char* fn;
    std::vector<u64> args;
    std::function<void(u8*)> setup;
    std::string what; // for a report
};

struct Answer {
    u32 r3 = 0;
    std::vector<u8> area;
};

// pspocket's function, as guest code calls it.
u32 ours(GuestThread& t, const char* name, const std::vector<u64>& args)
{
    const Function& fn = GuestThread::find(name);
    for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
    fn.fn(t.ctx, t.f.base());
    return static_cast<u32>(t.ctx.gpr[3]);
}

Answer run(u8* b, const Call& c, const std::function<u32(const char*, const std::vector<u64>&)>& call)
{
    reset(b);
    if (c.setup) c.setup(b);
    Answer a;
    a.r3 = call(c.fn, c.args);
    a.area.assign(b + kArea, b + kArea + kAreaSize);
    return a;
}

// ---- the calls ----

// Strings a parser sees: made-up URIs, paths and proxies over everything the parsers branch on.
std::vector<std::string> uris()
{
    return {
        "http://www.example.com/index.html",
        "https://user:pass@host.example.com:8443/a/b/../c/./d?x=1&y=2#frag",
        "HTTP://HOST/",
        "ftp://ftp.x.org/pub/file.txt",
        "svn+ssh://svn.example.com/repo",
        "imap://mail.example.com",
        "gopher://g.example.com:70/1",
        "rtsp://media.example.com/stream",
        "ssh://h",
        "SSHX://h",
        "unknown://h/p",
        "httpx://h/",
        "httpsx://h/",
        "Https://h/",
        "http:/one/slash",
        "http:///three/slashes",
        "//no/scheme",
        "host:80/path",
        "host",
        "host.",
        "  \t http://leading.spaces/",
        "\r\n\v\fhttp://ws/",
        "http://host:/emptyport",
        "http://host:65535/",
        "http://host:65536/",
        "http://host:65537/",
        "http://host:99999/",
        "http://host:123456/",
        "http://host:00080/",
        "http://host:000080/",
        "http://host:12a/",
        "http://host:080",
        "http://host:8080?q",
        "http://host:8080#f",
        "http://user@host/",
        "http://user:@host/",
        "http://:pass@host/",
        "http://:@host/",
        "http://@host/",
        "http://us er@host/",
        "http://a%20b:c;d&=+$,!~*'()@host/",
        "http://u@v@host/",
        "http://user:pa:ss@host/",
        "http://user/x@host/",
        "http://host.../trailing/dots",
        "http://.../",
        "http://./",
        "http://ho_st-1.x/",
        "http://host?q=1",
        "http://host#f",
        "http://host/a/b/c/../../d",
        "http://host/../../x",
        "http://host/./a/./b/",
        "http://host/a/..b/c",
        "http://host/a/..",
        "http://host/a/.",
        "http://host/a/../",
        "http://host/a/./",
        "http://host/a?x=/../y",
        "http://host/a#x/../y",
        "http://host/a/b?x/../y#z/./w",
        "http://host//double//slash/",
        "http://host/.../x",
        "http://host/a/b/c/../../../../x",
        "http://host/%2e%2e/x",
        "http://[::1]:80/",
        "mailto:someone@example.com",
        "1http://digits/first",
        ":empty.scheme/x",
        "+x://plus",
        "-x://minus",
        ".x://dot",
        "a.b+c-d://h/",
        "http://h\xC3\xA9llo/",
        "http://host/p\xFFq",
        "\x80http://x",
        "http\x80://x",
        "http://",
        "http:",
        "http:/",
        "http:host",
        "h",
        "/",
        "/a/b",
        "a/b",
        "?q",
        "#f",
        "",
        " ",
        "\t",
        "HTTPS://Host:443/",
        "https://host:80/",
        "http://host:443/",
        "http://host:0/",
        "file:///etc/passwd",
        "x:y:z",
        "http://host:80:90/",
        "http://user:pass@:80/",
        "proxy.example.com:3128",
        "user:pw@proxy:8080",
        "proxy:8080/ignored",
        "proxy:abc",
        "proxy:",
        "@proxy",
        "/a/b/c?d=e&f=g#h",
        "/a/./b/../c?x",
        "/?#",
        "?#",
        "#?",
        "a?b?c#d#e",
        "/index.html#",
        "/index.html?",
    };
}

std::string random_text(std::mt19937& rng, std::string_view alphabet, u32 max_len)
{
    std::uniform_int_distribution<u32> len(0, max_len), pick(0, static_cast<u32>(alphabet.size() - 1));
    std::string s;
    for (u32 n = len(rng); n--;) s += alphabet[pick(rng)];
    return s;
}

constexpr std::string_view kUriAlphabet = "aAzZ09:/@?#.%+-_~!*'();&=$, \t\r\nhtps:/.\x80\xFF";

// A parser with a pool: the full pool, pools of every size up to what it needs (and some), only the required size
// (no structure, or no pool), and nothing at all.
void parser_calls(std::vector<Call>& out, const char* fn, bool thorough, std::string text,
    const std::vector<u64>& extra = {}, bool with_length = false)
{
    const auto setup = [text](u8* b) { put(b, kIn, text); };
    const auto args = [&](u64 s, u64 pool, u64 size, u64 req, u64 parsed) {
        std::vector<u64> a{s, kIn};
        if (with_length) a.push_back(extra.empty() ? text.size() : extra[0]);
        a.push_back(pool);
        a.push_back(size);
        a.push_back(req);
        if (with_length) a.push_back(parsed);
        return a;
    };
    const std::string what = std::format("{}(\"{}\")", fn, text);
    out.push_back({fn, args(kStruct, kPool, 0x400, kRequired, kParsed), setup, what + " pool 0x400"});
    std::vector<u32> sizes = {0, 1, 2, 3, 7, 16, 31};
    if (thorough) {
        sizes.clear();
        for (u32 size = 0; size <= std::min<u32>(static_cast<u32>(text.size()) + 12, 0x60); size++) sizes.push_back(size);
    }
    for (const u32 size : sizes)
        out.push_back({fn, args(kStruct, kPool, size, kRequired, kParsed), setup, what + std::format(" pool {}", size)});
    out.push_back({fn, args(0, kPool, 0x400, kRequired, kParsed), setup, what + " no structure"});
    out.push_back({fn, args(kStruct, 0, 0x400, kRequired, kParsed), setup, what + " no pool"});
    out.push_back({fn, args(0, 0, 0, kRequired, 0), setup, what + " size only, no room"});
    out.push_back({fn, args(0, 0, 3, kRequired, 0), setup, what + " size only, room 3"});
    out.push_back({fn, args(0, 0, 0, 0, kParsed), setup, what + " nothing"});
    out.push_back({fn, args(kStruct, kPool, 0x400, 0, 0), setup, what + " no required"});
}

// Writes a CellHttpUri at kSrc from its parts (nullopt: a NULL pointer) and returns its address.
struct UriParts {
    std::optional<std::string> scheme, host, user, pass, path;
    u32 port = 0;
};

void put_uri(u8* b, u32 at, const UriParts& u)
{
    u32 text = kSrcText;
    const auto field = [&](u32 offset, const std::optional<std::string>& s) {
        if (!s) {
            ps_write32(b, at + offset, 0);
            return;
        }
        put(b, text, *s);
        ps_write32(b, at + offset, text);
        text += static_cast<u32>(s->size()) + 1;
    };
    field(0, u.scheme);
    field(4, u.host);
    field(8, u.user);
    field(12, u.pass);
    field(16, u.path);
    ps_write32(b, at + 20, u.port);
    ps_write32(b, at + 24, 0x11223344);
}

void put_strings(u8* b, u32 at, std::initializer_list<std::pair<u32, std::optional<std::string>>> fields)
{
    u32 text = kSrcText;
    for (const auto& [offset, s] : fields) {
        if (!s) {
            ps_write32(b, at + offset, 0);
            continue;
        }
        put(b, text, *s);
        ps_write32(b, at + offset, text);
        text += static_cast<u32>(s->size()) + 1;
    }
}

std::vector<UriParts> uri_parts()
{
    std::vector<UriParts> list;
    const std::optional<std::string> none;
    for (const auto& scheme : {none, std::optional<std::string>(""), std::optional<std::string>("http"),
             std::optional<std::string>("HTTPS"), std::optional<std::string>("https"), std::optional<std::string>("ftp"),
             std::optional<std::string>("httpx"), std::optional<std::string>("htt")})
        for (const u32 port : {0u, 80u, 443u, 8080u, 21u, 65535u, 0xFFFFFFFFu, 1000000u})
            list.push_back({scheme, "host.example.com", none, none, "/p/a?q#f", port});
    for (const auto& user : {none, std::optional<std::string>(""), std::optional<std::string>("user")})
        for (const auto& pass : {none, std::optional<std::string>(""), std::optional<std::string>("secret")})
            for (const auto& path : {none, std::optional<std::string>(""), std::optional<std::string>("/x")})
                list.push_back({"http", "h", user, pass, path, 8080});
    list.push_back({"http", "", none, none, none, 80});
    list.push_back({"http", none, none, none, none, 80});
    list.push_back({none, "h", "u", "p", "relative", 443});
    return list;
}

std::vector<std::string> headers()
{
    return {
        "Content-Type: text/html\r\n",
        "Content-Length:42",
        "Name:   value   \r\n",
        "Name: a\r\n b\r\n\tc\r\n",
        "Name: a\r\n  b  \r\n   c\r\nNext: x\r\n",
        "Name: \r\n",
        "Name:\r\n",
        "Name:",
        "Name: ",
        "Name:  ",
        ":x",
        "x:",
        "  Name: v",
        "\r\nName: v",
        "Name v",
        "Name: v\r\n\r\n",
        "Name: a\r\n \r\n",
        "Name: a\r\n ",
        "Name: a\n b",
        "Name: a\r\r\n b",
        "Name: a\n\n b",
        "Name: a\r\n\r\n b",
        "Name: a\rb",
        "Name: a\nb",
        "Name: x\xFFy\x80",
        "N\xC3\xA9: v",
        "Set-Cookie: a=b; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT\r\n",
        "A:B:C",
        "::",
        "Name : value",
        "Name\t:\tvalue\t",
        "Name: v\r\n\tw\r\n x \r\n",
        "Name: \r\n x",
        "",
        " ",
        "\r\n",
    };
}

std::vector<std::string> status_lines()
{
    return {
        "HTTP/1.1 200 OK\r\n",
        "HTTP/1.0 404 Not Found\r\n",
        "HTTP/1.1 200\r\n",
        "HTTP/1.1 200\n",
        "HTTP/1.1 200 \r\n",
        "HTTP/1.1 200  OK\r\n",
        "HTTP/1.1 200 \t OK \r\n",
        "HTTP/10.20 999999 x\n",
        "HTTP/1.1 abc OK\r\n",
        "/1.1 200 OK\r\n",
        "HTTP1.1 200 OK\r\n",
        "HTTP/1. 200 OK\r\n",
        "HTTP/.1 200 OK\r\n",
        "HTTP/1.1  200 OK\r\n",
        "HTTP/1.1 200 OK",
        "  HTTP/1.1 200 OK\r\n",
        "\r\nHTTP/1.1 200 OK\r\n",
        "HTTP/1.1 200 OK\r\n\r\nbody",
        "ICY/1.0 200 OK\n",
        "HTTP/1.1 4294967296 X\n",
        "HTTP/4294967297.1 1 X\n",
        "HTTP/1.1 200 OK\r",
        "HTTP/1.1 200 O\rK\r\n",
        "HTTP/1.1 2x0 OK\r\n",
        "HTTP/1.1200 OK\r\n",
        "HTTP/1.1 200OK\r\n",
        "HTTP/1.1 \r\n",
        "HTTP//1.1 200 OK\r\n",
        "HTTP/1.1/2 200 OK\r\n",
        "HTTP/\xFF.1 200 OK\r\n",
        "HTTP/1.\x80 200 OK\r\n",
        "HTTP/1.1 200 \xC3\xA9t\xC3\xA9\r\n",
        "",
        " ",
        "/",
        "HTTP/",
        "HTTP/1",
        "HTTP/1.",
        "HTTP/1.1",
        "HTTP/1.1 ",
        "HTTP/1.1 2",
    };
}

std::vector<Call> parser_calls_all(std::mt19937& rng)
{
    std::vector<Call> out;
    for (const std::string& u : uris())
        for (const char* fn : {"cellHttpUtilParseUri", "cellHttpUtilParseProxy", "cellHttpUtilParseUriPath"})
            parser_calls(out, fn, true, u);
    for (u32 i = 0; i < 400; i++) {
        const std::string u = random_text(rng, kUriAlphabet, 40);
        for (const char* fn : {"cellHttpUtilParseUri", "cellHttpUtilParseProxy", "cellHttpUtilParseUriPath"})
            parser_calls(out, fn, false, u);
    }
    // Headers and status lines, at their length and around it (the bytes past the NUL are the fill).
    const auto lines = [&](const char* fn, const std::vector<std::string>& texts, std::string_view alphabet, char stop) {
        for (const std::string& h : texts) {
            const u32 n = static_cast<u32>(h.size());
            for (const u32 len : {n, n + 1, n + 2, n ? n - 1 : 0u, n > 3 ? n - 3 : 0u, 0u, 0xFFFFFFFFu}) {
                // With no end, Sony's search for the ':' or '/' would run on through memory.
                if (len == 0xFFFFFFFFu && h.find(stop) == std::string::npos) continue;
                parser_calls(out, fn, true, h, {len}, true);
            }
        }
        for (u32 i = 0; i < 600; i++) {
            const std::string h = random_text(rng, alphabet, 40);
            const u32 n = static_cast<u32>(h.size());
            for (const u32 len : {n, n ? n - 1 : 0u})
                parser_calls(out, fn, false, h, {len}, true);
        }
    };
    lines("cellHttpUtilParseHeader", headers(), "Na:e \t\r\nv\x80\xFF:", ':');
    lines("cellHttpUtilParseStatusLine", status_lines(), "HTP/1.0 2\r\n \tOK\x80", '/');
    // A header's NUL inside its length, and text after the length.
    for (const std::string& h : {std::string("Na\0me: v\r\n", 10), std::string("Name\0: v", 8),
             std::string("Name: v\0al\r\n", 12), std::string("Name: a\r\n\0b", 11)}) {
        parser_calls(out, "cellHttpUtilParseHeader", true, h, {static_cast<u32>(h.size())}, true);
        parser_calls(out, "cellHttpUtilParseStatusLine", true, h, {static_cast<u32>(h.size())}, true);
    }
    // NULL strings.
    for (const char* fn : {"cellHttpUtilParseUri", "cellHttpUtilParseProxy", "cellHttpUtilParseUriPath"})
        out.push_back({fn, {kStruct, 0, kPool, 0x400, kRequired}, nullptr, std::format("{}(NULL)", fn)});
    for (const char* fn : {"cellHttpUtilParseHeader", "cellHttpUtilParseStatusLine"})
        out.push_back({fn, {kStruct, 0, 10, kPool, 0x400, kRequired, kParsed}, nullptr, std::format("{}(NULL)", fn)});
    return out;
}

std::vector<Call> builder_calls(std::mt19937& rng)
{
    std::vector<Call> out;
    // BuildUri: every flag combination, with the buffer just large enough, one byte short, and none.
    for (const UriParts& u : uri_parts()) {
        const auto setup = [u](u8* b) { put_uri(b, kSrc, u); };
        for (u32 flags = 0; flags < 16; flags++) {
            for (const u32 len : {0x400u, 0u, 1u, 20u, 27u, 28u, 29u, 34u, 35u, 36u})
                out.push_back({"cellHttpUtilBuildUri", {kSrc, kOut, len, kRequired, flags}, setup,
                    std::format("BuildUri flags {} len {}", flags, len)});
            out.push_back({"cellHttpUtilBuildUri", {kSrc, 0, 0, kRequired, flags}, setup, "BuildUri size only"});
            out.push_back({"cellHttpUtilBuildUri", {kSrc, kOut, 0x400, 0, flags}, setup, "BuildUri no required"});
        }
    }
    out.push_back({"cellHttpUtilBuildUri", {0, kOut, 0x400, kRequired, 0}, nullptr, "BuildUri(NULL)"});
    out.push_back({"cellHttpUtilBuildUri", {kSrc, kOut, 0x400, kRequired, 0xFFFFFFF0u},
        [](u8* b) { put_uri(b, kSrc, {"http", "h", "u", "p", "/x", 1}); }, "BuildUri high flags"});
    // BuildHeader and BuildRequestLine.
    const std::optional<std::string> none;
    for (const auto& name : {none, std::optional<std::string>(""), std::optional<std::string>("Host")})
        for (const auto& value : {none, std::optional<std::string>(""), std::optional<std::string>("example.com")}) {
            const auto setup = [name, value](u8* b) { put_strings(b, kSrc, {{0, name}, {4, value}}); };
            for (const u32 len : {0x400u, 0u, 3u, 4u, 5u, 9u, 10u, 18u, 19u, 20u, 21u})
                out.push_back({"cellHttpUtilBuildHeader", {kSrc, kOut, len, kRequired}, setup, "BuildHeader"});
            out.push_back({"cellHttpUtilBuildHeader", {kSrc, 0, 0, kRequired}, setup, "BuildHeader size only"});
            out.push_back({"cellHttpUtilBuildHeader", {kSrc, kOut, 0x400, 0}, setup, "BuildHeader no required"});
        }
    out.push_back({"cellHttpUtilBuildHeader", {0, kOut, 0x400, kRequired}, nullptr, "BuildHeader(NULL)"});
    for (const auto& method : {none, std::optional<std::string>("GET"), std::optional<std::string>("")})
        for (const auto& path : {none, std::optional<std::string>(""), std::optional<std::string>("/index.html")})
            for (const auto& proto : {none, std::optional<std::string>("HTTP"), std::optional<std::string>("")})
                for (const std::pair<u32, u32> version : std::vector<std::pair<u32, u32>>{{1, 1}, {0, 9}, {10, 0},
                         {4294967295u, 123456}}) {
                    const auto setup = [=](u8* b) {
                        put_strings(b, kSrc, {{0, method}, {4, path}, {8, proto}});
                        ps_write32(b, kSrc + 12, version.first);
                        ps_write32(b, kSrc + 16, version.second);
                    };
                    for (const u32 len : {0x400u, 0u, 13u, 14u, 15u, 25u, 26u, 27u})
                        out.push_back({"cellHttpUtilBuildRequestLine", {kSrc, kOut, len, kRequired}, setup,
                            "BuildRequestLine"});
                    out.push_back({"cellHttpUtilBuildRequestLine", {kSrc, 0, 0, kRequired}, setup,
                        "BuildRequestLine size only"});
                    out.push_back({"cellHttpUtilBuildRequestLine", {kSrc, kOut, 0x400, 0}, setup,
                        "BuildRequestLine no required"});
                }
    out.push_back({"cellHttpUtilBuildRequestLine", {0, kOut, 0x400, kRequired}, nullptr, "BuildRequestLine(NULL)"});
    (void)rng;
    return out;
}

std::vector<Call> copy_calls(std::mt19937& rng)
{
    std::vector<Call> out;
    const auto pools = [&](const char* fn, const std::function<void(u8*)>& setup, std::vector<u64> head,
                           std::string what) {
        const auto with = [&](u64 dest, u64 pool, u64 size, u64 req) {
            std::vector<u64> a = head;
            a.insert(a.begin(), dest);
            a.push_back(pool);
            a.push_back(size);
            a.push_back(req);
            return a;
        };
        out.push_back({fn, with(kStruct, kPool, 0x400, kRequired), setup, what + " pool 0x400"});
        for (u32 size = 0; size <= 0x50; size++)
            out.push_back({fn, with(kStruct, kPool, size, kRequired), setup, what + std::format(" pool {}", size)});
        out.push_back({fn, with(0, kPool, 0x400, kRequired), setup, what + " no structure"});
        out.push_back({fn, with(kStruct, 0, 0x400, kRequired), setup, what + " no pool"});
        out.push_back({fn, with(0, 0, 0, 0), setup, what + " nothing"});
        out.push_back({fn, with(kStruct, kPool, 0x400, 0), setup, what + " no required"});
    };
    for (const UriParts& u : uri_parts())
        pools("cellHttpUtilCopyUri", [u](u8* b) { put_uri(b, kSrc, u); }, {kSrc}, "CopyUri");
    pools("cellHttpUtilCopyUri", nullptr, {0}, "CopyUri(NULL)");
    const std::optional<std::string> none;
    for (const auto& name : {none, std::optional<std::string>(""), std::optional<std::string>("Accept")})
        for (const auto& value : {none, std::optional<std::string>(""), std::optional<std::string>("text/*"),
                 std::optional<std::string>("  padded  \r\n")}) {
            pools("cellHttpUtilCopyHeader", [=](u8* b) { put_strings(b, kSrc, {{0, name}, {4, value}}); }, {kSrc},
                "CopyHeader");
            for (const std::string add : {"", "x", "  y  ", "\tz\r\nmore", " \r", "a b c", "\r\n", "\xFF v"})
                pools("cellHttpUtilAppendHeaderValue",
                    [=](u8* b) {
                        put_strings(b, kSrc, {{0, name}, {4, value}});
                        put(b, kIn, add);
                    },
                    {kSrc, kIn}, std::format("AppendHeaderValue \"{}\"", add));
        }
    pools("cellHttpUtilCopyHeader", nullptr, {0}, "CopyHeader(NULL)");
    pools("cellHttpUtilAppendHeaderValue", [](u8* b) { put(b, kIn, "v"); }, {0, kIn}, "AppendHeaderValue(NULL)");
    for (const auto& protocol : {none, std::optional<std::string>(""), std::optional<std::string>("HTTP")})
        for (const auto& reason : {none, std::optional<std::string>(""), std::optional<std::string>("Not Found")})
            pools("cellHttpUtilCopyStatusLine",
                [=](u8* b) {
                    put_strings(b, kSrc, {{0, protocol}, {12, reason}});
                    ps_write32(b, kSrc + 4, 1);
                    ps_write32(b, kSrc + 8, 0);
                    ps_write32(b, kSrc + 16, 404);
                    ps_write32(b, kSrc + 20, 0x55667788);
                },
                {kSrc}, "CopyStatusLine");
    pools("cellHttpUtilCopyStatusLine", nullptr, {0}, "CopyStatusLine(NULL)");
    // MergeUriPath: base paths and relative or absolute paths.
    const std::vector<std::optional<std::string>> bases = {none, std::string(""), std::string("/"),
        std::string("/a/b/c"), std::string("/a/b/"), std::string("/a/b/c?q=/x#f"), std::string("/a/b#f/g"),
        std::string("noslash"), std::string("rel/dir/file"), std::string("/a/../b/./c"), std::string("?q"),
        std::string("#f/x")};
    const std::vector<std::string> paths = {"", "x", "/x", "../x", "../../../x", "./x", "x/./y/../z", "/a/../b",
        "x?y=../z", "x#../y", "..", ".", "../", "./", "x/", "//x", "a/b/c/d/e/f", "?q", "#f"};
    for (const auto& base : bases)
        for (const std::string& p : paths) {
            const auto setup = [=](u8* b) {
                put_uri(b, kSrc, {"http", "host", "u", "pw", base, 8080});
                put(b, kIn, p);
            };
            const std::string what = std::format("MergeUriPath base \"{}\" path \"{}\"", base.value_or("NULL"), p);
            out.push_back({"cellHttpUtilMergeUriPath", {kStruct, kSrc, kIn, kPool, 0x400, kRequired}, setup, what});
            for (u32 size = 0; size <= 0x30; size++)
                out.push_back({"cellHttpUtilMergeUriPath", {kStruct, kSrc, kIn, kPool, size, kRequired}, setup,
                    what + std::format(" pool {}", size)});
            out.push_back({"cellHttpUtilMergeUriPath", {0, kSrc, kIn, 0, 0, kRequired}, setup, what + " size only"});
        }
    out.push_back({"cellHttpUtilMergeUriPath", {kStruct, kSrc, 0, kPool, 0x400, kRequired}, nullptr,
        "MergeUriPath(NULL path)"});
    out.push_back({"cellHttpUtilMergeUriPath", {kStruct, 0, kIn, kPool, 0x400, kRequired},
        [](u8* b) { put(b, kIn, "x"); }, "MergeUriPath(NULL src)"});
    out.push_back({"cellHttpUtilMergeUriPath", {0, kSrc, kIn, 0, 0, 0},
        [](u8* b) { put_uri(b, kSrc, {"http", "h", std::nullopt, std::nullopt, "/a/", 80}); put(b, kIn, "x"); }, "MergeUriPath nothing"});
    // SweepPath: into another buffer and in place, at the string's size and shorter.
    std::vector<std::string> sweeps = {"/a/b/../c/./d", "/../x", "/./", "/a/..", "/a/.", "/a/b/c/../../../../",
        "relative/../x", "/a?b/../c", "/a#b/./c", "/a/b?c#d/../e", "//", "/", "", "/.../..../", "/a/../../b/../c",
        "/x/./././y", "/x/../y/../z/..", ".", "./x", "../x"};
    for (u32 i = 0; i < 200; i++) sweeps.push_back("/" + random_text(rng, "ab./?#", 24));
    for (const std::string& s : sweeps) {
        const u32 n = static_cast<u32>(s.size());
        for (const u32 size : {n + 1, n, n ? n - 1 : 0u, 1u, 0u, n + 3}) {
            const auto setup = [s](u8* b) { put(b, kIn, s); };
            out.push_back({"cellHttpUtilSweepPath", {kOut, kIn, size}, setup, std::format("SweepPath(\"{}\", {})", s, size)});
            out.push_back({"cellHttpUtilSweepPath", {kIn, kIn, size}, setup,
                std::format("SweepPath in place(\"{}\", {})", s, size)});
        }
    }
    out.push_back({"cellHttpUtilSweepPath", {0, kIn, 4}, nullptr, "SweepPath(NULL dst)"});
    out.push_back({"cellHttpUtilSweepPath", {kOut, 0, 4}, nullptr, "SweepPath(NULL src)"});
    return out;
}

std::vector<Call> escape_calls(std::mt19937& rng)
{
    std::vector<Call> out;
    std::string every;
    for (u32 c = 1; c < 256; c++) every += static_cast<char>(c);
    std::vector<std::string> texts = {"hello world", "a b+c%d&e=f/g?h#i", "~!*'()-._", every, std::string("x\0y", 3),
        "%41%42%43", "%4", "%", "%%", "%zz", "%g0", "%0g", "100%25", "a+b+c", "%2B%2b", "%e3%81%82", "%E3%81%82",
        "", " ", "+"};
    for (u32 i = 0; i < 200; i++) texts.push_back(random_text(rng, "aZ9 %+-._*~/\x80\xFF" "0123456789abcdefABCDEF", 30));
    for (const std::string& s : texts) {
        const u32 n = static_cast<u32>(s.size());
        const auto setup = [s](u8* b) { put(b, kIn, s); };
        for (const char* fn : {"cellHttpUtilEscapeUri", "cellHttpUtilFormUrlEncode"}) {
            for (const u32 in_size : {n, n + 1, n ? n - 1 : 0u, 1u})
                for (const u32 out_size : {0x800u, 0u, 1u, n, n + 1, 3 * n, 3 * n + 1, 2 * n})
                    out.push_back({fn, {kOut, out_size, kIn, in_size, kRequired}, setup,
                        std::format("{}(\"{}\", {}, {})", fn, s, in_size, out_size)});
            out.push_back({fn, {0, 0, kIn, n, kRequired}, setup, std::format("{} size only", fn)});
            out.push_back({fn, {0, 0, kIn, n, 0}, setup, std::format("{} nothing", fn)});
            out.push_back({fn, {kOut, 0x800, kIn, n, 0}, setup, std::format("{} no required", fn)});
            out.push_back({fn, {kOut, 0x800, 0, n, kRequired}, setup, std::format("{}(NULL)", fn)});
            out.push_back({fn, {kOut, 0x800, kIn, 0xFFFFFFFFu, kRequired}, setup, std::format("{} wrapping", fn)});
            out.push_back({fn, {kOut, 0x800, kIn, 0u - kIn, kRequired}, setup, std::format("{} to the end", fn)});
        }
        for (const char* fn : {"cellHttpUtilUnescapeUri", "cellHttpUtilFormUrlDecode"}) {
            for (const u32 size : {0x800u, 0u, 1u, n, n + 1, n ? n - 1 : 0u, n / 3, n / 3 + 1, n / 2})
                out.push_back({fn, {kOut, size, kIn, kRequired}, setup, std::format("{}(\"{}\", {})", fn, s, size)});
            out.push_back({fn, {0, 0, kIn, kRequired}, setup, std::format("{} size only", fn)});
            out.push_back({fn, {0, 0, kIn, 0}, setup, std::format("{} nothing", fn)});
            out.push_back({fn, {kOut, 0x800, kIn, 0}, setup, std::format("{} no required", fn)});
            out.push_back({fn, {kOut, 0x800, 0, kRequired}, setup, std::format("{}(NULL)", fn)});
        }
    }
    return out;
}

std::vector<Call> base64_calls(std::mt19937& rng)
{
    std::vector<Call> out;
    std::uniform_int_distribution<u32> byte(0, 255);
    for (u32 len = 0; len <= 40; len++)
        for (u32 k = 0; k < 4; k++) {
            std::string data;
            for (u32 i = 0; i < len; i++) data += static_cast<char>(byte(rng));
            const auto setup = [data](u8* b) { put(b, kIn, data, false); };
            out.push_back({"cellHttpUtilBase64Encoder", {kOut, kIn, len}, setup, std::format("Base64Encoder {}", len)});
        }
    out.push_back({"cellHttpUtilBase64Encoder", {0, kIn, 3}, nullptr, "Base64Encoder(NULL out)"});
    out.push_back({"cellHttpUtilBase64Encoder", {kOut, 0, 3}, nullptr, "Base64Encoder(NULL in)"});
    std::vector<std::string> codes = {"", "QQ==", "QUI=", "QUJD", "QUJDRA==", "Q===", "=AAA", "A=AA", "AA=A", "AAA=",
        "AA==", "====", "QQ=x", "QUJ=", "QUJDRA=", "QUJ", "Zm9vYmFy", "Zm9vYmE=", "Zm9vYg==", "////", "++++", "AAAA",
        "aGVsbG8gd29ybGQ=", "a!bc", "ab\x80" "c", "\xE7\xE8\xF7\xF8", "\xFF\xFF\xFF\xFF", "\x80\x80\x80\x80",
        "\xF6\xF6\xF6\xF6", "\xE8\xE9\xEA\xEB" "QQ==", "QQ==QQ==", "AB CD", "ABCD\nEFGH"};
    for (u32 i = 0; i < 300; i++) {
        std::string s = random_text(rng, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=", 24);
        s.resize(s.size() & ~usize{3});
        codes.push_back(s);
    }
    for (u32 i = 0; i < 300; i++) {
        std::string s;
        for (u32 n = 4 * (1 + byte(rng) % 6); n--;) s += static_cast<char>(byte(rng));
        codes.push_back(s);
    }
    for (const std::string& s : codes) {
        const u32 n = static_cast<u32>(s.size());
        const auto setup = [s](u8* b) { put(b, kIn, s); };
        for (const u32 len : {n, n + 1, n ? n - 1 : 0u, n + 4, n & ~3u})
            out.push_back({"cellHttpUtilBase64Decoder", {kOut, kIn, len}, setup,
                std::format("Base64Decoder(\"{}\", {})", s, len)});
    }
    out.push_back({"cellHttpUtilBase64Decoder", {0, kIn, 4}, nullptr, "Base64Decoder(NULL out)"});
    out.push_back({"cellHttpUtilBase64Decoder", {kOut, 0, 4}, nullptr, "Base64Decoder(NULL in)"});
    out.push_back({"cellHttpUtilBase64Decoder", {0, kIn, 5}, nullptr, "Base64Decoder(NULL out, bad length)"});
    return out;
}

// Runs every call on both sides; reports the first differences.
void compare(GuestThread& t, const FirmwareLibrary& sony, const std::vector<Call>& calls, usize& total)
{
    u8* b = t.f.base();
    usize differ = 0;
    for (const Call& c : calls) {
        const Answer mine = run(b, c, [&](const char* fn, const std::vector<u64>& a) { return ours(t, fn, a); });
        const Answer theirs = run(b, c, [&](const char* fn, const std::vector<u64>& a) {
            const auto it = sony.module->functions.find({"cellHttpUtil", recomp::nid_of(fn)});
            REQUIRE(it != sony.module->functions.end());
            std::array<u64, 8> r{};
            std::copy(a.begin(), a.end(), r.begin());
            return static_cast<u32>(call_guest(t.ctx, it->second, {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]}));
        });
        total++;
        if (mine.r3 == theirs.r3 && mine.area == theirs.area) continue;
        usize at = 0;
        while (at < mine.area.size() && mine.area[at] == theirs.area[at]) at++;
        if (++differ <= 40)
            UNSCOPED_INFO(std::format("{}: {:#x} (pspocket) vs {:#x} (Sony){}", c.what, mine.r3, theirs.r3,
                at < mine.area.size() ? std::format("; area differs first at {:#x}: {:#04x} vs {:#04x}", kArea + at,
                                            mine.area[at], theirs.area[at])
                                      : std::string()));
    }
    INFO(std::format("{} of {} calls differ", differ, calls.size()));
    CHECK(differ == 0);
}

// ---- without the firmware ----

struct Local {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    Local() { reset(b); }
    u32 call(const char* fn, std::vector<u64> args) { return ours(t, fn, args); }
};

} // namespace

TEST_CASE("cellHttpUtil parses URIs as libhttp does", "[hle][httputil]")
{
    Local l;
    put(l.b, kIn, "  https://user:pw@Host.example.com:8443/a/b/../c/./d?x=1#f");
    CHECK(l.call("cellHttpUtilParseUri", {0, kIn, 0, 0, kRequired}) == OK);
    const u32 required = ps_read32(l.b, kRequired);
    CHECK(required == 6 + 5 + 3 + 17 + 20); // the path's size before it is swept
    CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, kPool, required - 1, kRequired}) == NO_MEMORY);
    CHECK(ps_read32(l.b, kStruct + 4) == 0); // cleared again
    CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, kPool, required, kRequired}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 0)) == "https");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "Host.example.com");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 8)) == "user");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 12)) == "pw");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 16)) == "/a/c/d?x=1#f");
    CHECK(ps_read32(l.b, kStruct + 20) == 8443);
    // Ports a scheme implies, by a case-insensitive prefix.
    for (const auto& [text, port] : std::vector<std::pair<std::string, u32>>{{"http://h/", 80}, {"HTTPS://h/", 443},
             {"ftp://h", 21}, {"gopher://h", 70}, {"svn+ssh://h", 22}, {"httpx://h", 80}, {"news://h", 0},
             {"//h/", 80}, {"http://h:/", 80}, {"http://h:65536/", 65536}}) {
        put(l.b, kIn, text);
        INFO(text);
        CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, kPool, 0x400, 0}) == OK);
        CHECK(ps_read32(l.b, kStruct + 20) == port);
    }
    // A host's trailing dots are left out and not consumed: what follows is the path, the port the scheme's.
    put(l.b, kIn, "http://host.:81/x");
    CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, kPool, 0x400, 0}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "host");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 16)) == ".:81/x");
    CHECK(ps_read32(l.b, kStruct + 20) == 80);
    for (const char* bad : {"http://h:65537/", "http://h:123456/", "http://h:12x", "", " \t"}) {
        put(l.b, kIn, bad);
        INFO(bad);
        CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, kPool, 0x400, kRequired}) == INVALID_URI);
    }
    CHECK(l.call("cellHttpUtilParseUri", {kStruct, 0, kPool, 0x400, kRequired}) == NO_STRING);
    CHECK(l.call("cellHttpUtilParseUri", {kStruct, kIn, 0, 0x400, 0}) == NO_BUFFER);
    // A proxy: 8080 by default, no scheme or path.
    put(l.b, kIn, "u:p@proxy.example.com/ignored");
    CHECK(l.call("cellHttpUtilParseProxy", {kStruct, kIn, kPool, 0x400, kRequired}) == OK);
    CHECK(ps_read32(l.b, kStruct + 0) == 0);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "proxy.example.com");
    CHECK(ps_read32(l.b, kStruct + 16) == 0);
    CHECK(ps_read32(l.b, kStruct + 20) == 8080);
    // A path: the query and fragment keep their '?' and '#'.
    put(l.b, kIn, "/a/./b/../c?q=1#frag");
    CHECK(l.call("cellHttpUtilParseUriPath", {kStruct, kIn, kPool, 0x400, kRequired}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 0)) == "/a/c");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "?q=1");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 8)) == "#frag");
    CHECK(ps_read32(l.b, kRequired) == 12 + 5 + 6);
}

TEST_CASE("cellHttpUtil parses headers and status lines as libhttp does", "[hle][httputil]")
{
    Local l;
    const std::string h = "Name:  a  \r\n  b\r\n\tc \r\nNext: x";
    put(l.b, kIn, h);
    CHECK(l.call("cellHttpUtilParseHeader", {kStruct, kIn, h.size(), kPool, 0x400, kRequired, kParsed}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 0)) == "Name");
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "a b c");
    CHECK(ps_read32(l.b, kRequired) == 5 + 6);
    CHECK(ps_read32(l.b, kParsed) == h.find("Next") - 7); // from the value's start
    const std::string bad[] = {":x", "Name v"};
    for (const std::string& s : bad) {
        put(l.b, kIn, s);
        CHECK(l.call("cellHttpUtilParseHeader", {kStruct, kIn, s.size(), kPool, 0x400, kRequired, kParsed}) ==
              INVALID_HEADER);
    }
    const std::string s = "HTTP/1.1 404  Not Found\r\nX";
    put(l.b, kIn, s);
    CHECK(l.call("cellHttpUtilParseStatusLine", {kStruct, kIn, s.size(), kPool, 0x400, kRequired, kParsed}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 0)) == "HTTP");
    CHECK(ps_read32(l.b, kStruct + 4) == 1);
    CHECK(ps_read32(l.b, kStruct + 8) == 1);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 12)) == "Not Found\r"); // measured before the extra space, as Sony's
    CHECK(ps_read32(l.b, kStruct + 16) == 404);
    CHECK(ps_read32(l.b, kParsed) == 12); // from the phrase's start
    // No space after the code: the phrase is looked for as far on as the version took ("1.1 ").
    const std::string t = "HTTP/1.1 200\r\n";
    put(l.b, kIn, t);
    CHECK(l.call("cellHttpUtilParseStatusLine", {kStruct, kIn, t.size(), kPool, 0x400, kRequired, kParsed}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 12)).empty());
    CHECK(ps_read32(l.b, kParsed) == 1);
    put(l.b, kIn, "HTTP/1.1 200\n");
    CHECK(l.call("cellHttpUtilParseStatusLine", {kStruct, kIn, 13, kPool, 0x400, kRequired, kParsed}) ==
          INVALID_RESPONSE);
}

TEST_CASE("cellHttpUtil builds, copies and escapes as libhttp does", "[hle][httputil]")
{
    Local l;
    put_uri(l.b, kSrc, {std::nullopt, "h", "u", "p", "/x", 8080});
    CHECK(l.call("cellHttpUtilBuildUri", {kSrc, kOut, 0x400, kRequired, 0}) == OK);
    CHECK(cstr(l.b, kOut) == "http://u:p@h:8080/x");
    CHECK(ps_read32(l.b, kRequired) == 20);
    CHECK(l.call("cellHttpUtilBuildUri", {kSrc, kOut, 19, kRequired, 0}) == INSUFFICIENT);
    CHECK(l.call("cellHttpUtilBuildUri", {kSrc, kOut, 0x400, 0, 1 | 4 | 8}) == OK);
    CHECK(cstr(l.b, kOut) == "u@h:8080");
    put_uri(l.b, kSrc, {"news", "h", std::nullopt, std::nullopt, std::nullopt, 0});
    CHECK(l.call("cellHttpUtilBuildUri", {kSrc, kOut, 0x400, 0, 0}) == OK);
    CHECK(cstr(l.b, kOut) == "news://h:0");
    put_strings(l.b, kSrc, {{0, "GET"}, {4, ""}, {8, "HTTP"}});
    ps_write32(l.b, kSrc + 12, 1);
    ps_write32(l.b, kSrc + 16, 0);
    CHECK(l.call("cellHttpUtilBuildRequestLine", {kSrc, kOut, 0x400, kRequired}) == OK);
    CHECK(std::string(reinterpret_cast<char*>(l.b + kOut), 16) == "GET / HTTP/1.0\r\n");
    CHECK(ps_read32(l.b, kRequired) == 16);
    put_strings(l.b, kSrc, {{0, "Accept"}, {4, "a"}});
    put(l.b, kIn, "  b\r\nc");
    CHECK(l.call("cellHttpUtilAppendHeaderValue", {kStruct, kSrc, kIn, kPool, 0x400, kRequired}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 4)) == "a b");
    // MergeUriPath.
    put_uri(l.b, kSrc, {"http", "h", std::nullopt, std::nullopt, "/a/b/c?q", 80});
    put(l.b, kIn, "../d/./e");
    CHECK(l.call("cellHttpUtilMergeUriPath", {kStruct, kSrc, kIn, kPool, 0x400, kRequired}) == OK);
    CHECK(cstr(l.b, ps_read32(l.b, kStruct + 16)) == "/a/d/e");
    // SweepPath, Escape and Unescape.
    put(l.b, kIn, "/a/b/../../c/./d");
    CHECK(l.call("cellHttpUtilSweepPath", {kOut, kIn, 17}) == OK);
    CHECK(cstr(l.b, kOut) == "/c/d");
    put(l.b, kIn, "a b~*");
    CHECK(l.call("cellHttpUtilEscapeUri", {kOut, 0x100, kIn, 5, kRequired}) == OK);
    CHECK(cstr(l.b, kOut) == "a%20b%7E*");
    CHECK(l.call("cellHttpUtilFormUrlEncode", {kOut, 0x100, kIn, 5, kRequired}) == OK);
    CHECK(cstr(l.b, kOut) == "a+b%7E*");
    CHECK(ps_read32(l.b, kRequired) == 8);
    CHECK(l.call("cellHttpUtilFormUrlEncode", {kOut, 7, kIn, 5, kRequired}) == NO_MEMORY);
    put(l.b, kIn, "a+b%41%6a");
    CHECK(l.call("cellHttpUtilFormUrlDecode", {kOut, 0x100, kIn, kRequired}) == OK);
    CHECK(cstr(l.b, kOut) == "a bAj");
    CHECK(l.call("cellHttpUtilUnescapeUri", {kOut, 0x100, kIn, kRequired}) == OK);
    CHECK(cstr(l.b, kOut) == "a+bAj");
    put(l.b, kIn, "%4g");
    CHECK(l.call("cellHttpUtilUnescapeUri", {kOut, 0x100, kIn, kRequired}) == INVALID_URI);
    // Base64.
    put(l.b, kIn, "foobar", false);
    CHECK(l.call("cellHttpUtilBase64Encoder", {kOut, kIn, 4}) == 8);
    CHECK(std::string(reinterpret_cast<char*>(l.b + kOut), 8) == "Zm9vYg==");
    put(l.b, kIn, "Zm9vYg==");
    CHECK(l.call("cellHttpUtilBase64Decoder", {kOut, kIn, 8}) == 4);
    CHECK(std::string(reinterpret_cast<char*>(l.b + kOut), 4) == "foob");
    CHECK(l.call("cellHttpUtilBase64Decoder", {kOut, kIn, 6}) == INVALID_LENGTH);
    CHECK(l.call("cellHttpUtilBase64Decoder", {kOut, kIn, 0}) == INVALID_LENGTH);
    put(l.b, kIn, "Zm9v!g==");
    CHECK(l.call("cellHttpUtilBase64Decoder", {kOut, kIn, 8}) == INVALID_CHARACTER);
    // A pointer to memory that is not there is refused, not a host fault.
    CHECK(l.call("cellHttpUtilBuildUri", {0x00000100, kOut, 0x400, kRequired, 0}) == NO_BUFFER);
    (void)INVALID_REQUEST;
}

TEST_CASE("cellHttpUtil against libhttp", "[hle][lle][httputil]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellHttpUtil", false);
    std::mt19937 rng(20261008);
    usize total = 0;
    SECTION("parsers") { compare(t, sony, parser_calls_all(rng), total); }
    SECTION("builders") { compare(t, sony, builder_calls(rng), total); }
    SECTION("copies and paths") { compare(t, sony, copy_calls(rng), total); }
    SECTION("escapes") { compare(t, sony, escape_calls(rng), total); }
    SECTION("base64") { compare(t, sony, base64_calls(rng), total); }
    CHECK(total > 0);
}
