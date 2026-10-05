#pragma once

// Code signing on the device (route B, D-147; stage 1's go/no-go, D-150): the signature iOS checks before it maps a
// library's code. A CodeDirectory (version 0x20400, SHA-256 over 4 KB pages, the team ID, the exec segment), an empty
// requirement set, and a detached CMS SignedData over the CodeDirectory, made with the user's development identity.
// Without an identity the signature is ad hoc (macOS runs ad hoc code; iOS does not).
//
// The CMS is written here (DER) and signed through the platform (Security.framework on Apple systems): no OpenSSL,
// whose licences do not combine with GPLv2 (D-001). zsign and ChOma (MIT) were the reference for the layout, Apple's
// codesign and TN3126 for the checks.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::codesign {

// A development identity: its certificate, the chain up to Apple's root (WWDR G3, Apple Root CA), and a function that
// signs bytes with its private key (RSA PKCS#1 v1.5 with SHA-256).
struct SigningKey {
    std::vector<u8> certificate;              // DER
    std::vector<std::vector<u8>> chain;       // DER, intermediate first
    std::string team_id;                      // the certificate's OU
    std::function<std::optional<std::vector<u8>>(const std::vector<u8>& data)> sign_sha256_rsa;
};

struct SignOptions {
    std::string identifier;            // the code identifier; route B uses the app's bundle ID
    const SigningKey* key = nullptr;   // nullptr: ad hoc
    u64 signing_time = 0;              // seconds since 1970 for the CMS (0: now)
};

// Replaces the code signature of a 64-bit Mach-O that has an LC_CODE_SIGNATURE (answer_dylib makes one; linkers
// reserve one) with a new one over all its pages, and resizes __LINKEDIT and the file to fit.
bool sign(std::vector<u8>& macho, const SignOptions& options, std::string* error = nullptr);

// The SHA-256 of the signature's CodeDirectory (its CDHash in full), for logs and tests; nullopt without one.
std::optional<std::vector<u8>> code_directory_hash(const std::vector<u8>& macho);

// Platform identities (identity_apple.mm). A .p12 the app was given (SecPKCS12Import) with extra DER certificates for
// the chain (WWDR, root) when the .p12 lacks them.
std::optional<SigningKey> key_from_p12(const std::vector<u8>& p12, const std::string& password,
    const std::vector<std::vector<u8>>& extra_chain, std::string* error = nullptr);
// macOS: the first identity in the keychains whose certificate's subject contains `name` ("Apple Development: ...");
// its chain from a trust evaluation. Using its key may show the keychain's permission dialog.
std::optional<SigningKey> key_from_keychain(const std::string& name, std::string* error = nullptr);
// macOS: writes that identity to out_dir for the phone (scripts/ios.sh push-signing-identity): identity.p12 under a
// random password, the password in identity.pass (both readable by the user only), and its chain as chain-N.cer. macOS
// asks the user once to allow the export. Never into the repository (CLAUDE.md rule 1); never printed.
bool export_identity(const std::string& name, const std::string& out_dir, std::string* error = nullptr);
// A throwaway RSA identity with a self-signed certificate (tests: the CMS verifies against it, not Apple's root).
std::optional<SigningKey> key_for_tests(const std::string& team_id, std::string* error = nullptr);

} // namespace pspocket::codesign
