// pspocket-codesign: route B's signer on the Mac (D-150), for its go/no-go test and to check it against Apple's tools.
//
//   pspocket-codesign answer OUT.dylib [--platform ios|macos] [--answer N] [--identifier ID] [--min-os 17.0]
//                     [--identity NAME | --p12 FILE --password-file FILE [--chain CERT.der]...]
//   pspocket-codesign sign IN OUT --identifier ID [--identity NAME | --p12 ... ]
//
// `answer` writes the minimal library the phone's probe loads (ps_answer() returns N, 42 by default). Without an
// identity the signature is ad hoc. --identity takes the first keychain identity whose name contains NAME (macOS asks
// once to let this tool use the key); --p12 a .p12 with its password in a file (never on the command line). Check the
// result with `codesign -dvvvv -r- OUT` and `codesign --verify --verbose=4 OUT`.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/codesign/codesign.hpp"
#include "pspocket/codesign/macho.hpp"

using namespace pspocket;
using namespace pspocket::codesign;

namespace {

int usage()
{
    std::fprintf(stderr,
        "usage: pspocket-codesign answer OUT.dylib [--platform ios|macos] [--answer N] [--identifier ID] [--min-os X.Y]\n"
        "                         [--identity NAME | --p12 FILE --password-file FILE [--chain CERT.der]...]\n"
        "       pspocket-codesign sign IN OUT --identifier ID [--identity NAME | --p12 ...]\n"
        "       pspocket-codesign export-identity NAME OUT_DIR\n");
    return 64;
}

bool read_file(const std::string& path, std::vector<u8>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const std::vector<u8>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return usage();
    const std::string mode = argv[1];
    if (mode == "export-identity") { // export-identity NAME OUT_DIR (scripts/ios.sh push-signing-identity)
        if (argc != 4) return usage();
        std::string error;
        if (!export_identity(argv[2], argv[3], &error)) {
            std::fprintf(stderr, "pspocket-codesign: %s\n", error.c_str());
            return 1;
        }
        std::printf("exported to %s\n", argv[3]);
        return 0;
    }
    std::string out_path, in_path, identifier = "com.artzymeri.pspocket.testhost", identity, p12, password_file;
    std::vector<std::string> chain_files;
    Platform platform = Platform::IOS;
    u32 answer = 42, min_os = 0x00110000;
    int i = 2;
    if (mode == "answer") {
        out_path = argv[i++];
    } else if (mode == "sign" && argc >= 4) {
        in_path = argv[i++];
        out_path = argv[i++];
    } else {
        return usage();
    }
    for (; i < argc; i++) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--platform") {
            const std::string v = value();
            if (v == "ios") platform = Platform::IOS;
            else if (v == "macos") platform = Platform::MacOS;
            else return usage();
        } else if (a == "--answer") {
            answer = static_cast<u32>(std::strtoul(value().c_str(), nullptr, 0));
        } else if (a == "--identifier") {
            identifier = value();
        } else if (a == "--min-os") {
            const std::string v = value();
            unsigned major = 0, minor = 0;
            if (std::sscanf(v.c_str(), "%u.%u", &major, &minor) < 1) return usage();
            min_os = major << 16 | minor << 8;
        } else if (a == "--identity") {
            identity = value();
        } else if (a == "--p12") {
            p12 = value();
        } else if (a == "--password-file") {
            password_file = value();
        } else if (a == "--chain") {
            chain_files.push_back(value());
        } else {
            return usage();
        }
    }

    std::vector<u8> macho;
    if (mode == "answer") {
        macho = answer_dylib("ps_answer", answer, platform, min_os, "@rpath/answer.dylib");
    } else if (!read_file(in_path, macho)) {
        std::fprintf(stderr, "pspocket-codesign: cannot read %s\n", in_path.c_str());
        return 66;
    }

    std::optional<SigningKey> key;
    std::string error;
    if (!identity.empty()) {
        key = key_from_keychain(identity, &error);
    } else if (!p12.empty()) {
        std::vector<u8> data, password_bytes;
        if (!read_file(p12, data) || !read_file(password_file, password_bytes)) {
            std::fprintf(stderr, "pspocket-codesign: cannot read %s or its password file\n", p12.c_str());
            return 66;
        }
        std::string password(password_bytes.begin(), password_bytes.end());
        while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();
        std::vector<std::vector<u8>> chain;
        for (const auto& c : chain_files) {
            std::vector<u8> der;
            if (!read_file(c, der)) {
                std::fprintf(stderr, "pspocket-codesign: cannot read %s\n", c.c_str());
                return 66;
            }
            chain.push_back(std::move(der));
        }
        key = key_from_p12(data, password, chain, &error);
    }
    if ((!identity.empty() || !p12.empty()) && !key) {
        std::fprintf(stderr, "pspocket-codesign: %s\n", error.c_str());
        return 1;
    }
    SignOptions options;
    options.identifier = identifier;
    options.key = key ? &*key : nullptr;
    if (!sign(macho, options, &error)) {
        std::fprintf(stderr, "pspocket-codesign: %s\n", error.c_str());
        return 1;
    }
    if (!write_file(out_path, macho)) {
        std::fprintf(stderr, "pspocket-codesign: cannot write %s\n", out_path.c_str());
        return 73;
    }
    std::printf("%s: %zu bytes, %s, identifier %s%s%s\n", out_path.c_str(), macho.size(),
        key ? "signed" : "ad hoc", identifier.c_str(), key ? ", team " : "", key ? key->team_id.c_str() : "");
    return 0;
}
