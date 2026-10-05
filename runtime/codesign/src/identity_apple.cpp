// Signing identities through Security.framework (Apple platforms): the private key never leaves it; we hand it the
// bytes to sign. C APIs only, so this is plain C++.

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "der.hpp"
#include "pspocket/codesign/codesign.hpp"

namespace pspocket::codesign {
namespace {

using der::Bytes;

struct CfDeleter {
    void operator()(CFTypeRef r) const
    {
        if (r) CFRelease(r);
    }
};
template <class T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, CfDeleter>;

Bytes bytes_of(CFDataRef d) { return Bytes(CFDataGetBytePtr(d), CFDataGetBytePtr(d) + CFDataGetLength(d)); }

std::string error_text(CFErrorRef e)
{
    if (!e) return "unknown error";
    Cf<CFStringRef> s(CFErrorCopyDescription(e));
    char buf[512] = {};
    if (s) CFStringGetCString(s.get(), buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

// The signing function for a key: RSA PKCS#1 v1.5 over SHA-256 of the bytes.
std::function<std::optional<Bytes>(const Bytes&)> signer_for(SecKeyRef key)
{
    CFRetain(key);
    std::shared_ptr<const void> hold(key, [](const void* k) { CFRelease(k); });
    return [hold](const Bytes& data) -> std::optional<Bytes> {
        Cf<CFDataRef> in(CFDataCreate(nullptr, data.data(), static_cast<CFIndex>(data.size())));
        CFErrorRef e = nullptr;
        Cf<CFDataRef> sig(SecKeyCreateSignature(static_cast<SecKeyRef>(const_cast<void*>(hold.get())),
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, in.get(), &e));
        if (e) CFRelease(e);
        if (!sig) return std::nullopt;
        return bytes_of(sig.get());
    };
}

Bytes certificate_bytes(SecCertificateRef c)
{
    Cf<CFDataRef> d(SecCertificateCopyData(c));
    return d ? bytes_of(d.get()) : Bytes{};
}

// The chain above `leaf` as a trust evaluation finds it (the keychains' intermediates and roots).
std::vector<Bytes> chain_of(SecCertificateRef leaf)
{
    std::vector<Bytes> chain;
    Cf<SecPolicyRef> policy(SecPolicyCreateBasicX509());
    SecTrustRef trust_raw = nullptr;
    if (SecTrustCreateWithCertificates(leaf, policy.get(), &trust_raw) != errSecSuccess) return chain;
    Cf<SecTrustRef> trust(trust_raw);
    (void)SecTrustEvaluateWithError(trust.get(), nullptr); // the chain is wanted even when the leaf is not trusted
    Cf<CFArrayRef> certs(SecTrustCopyCertificateChain(trust.get()));
    if (!certs) return chain;
    for (CFIndex i = 1; i < CFArrayGetCount(certs.get()); i++) {
        chain.push_back(certificate_bytes(static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(certs.get(), i)))));
    }
    return chain;
}

std::optional<SigningKey> key_from_identity(SecIdentityRef identity, std::vector<Bytes> chain, std::string* error)
{
    SecCertificateRef cert_raw = nullptr;
    SecKeyRef key_raw = nullptr;
    if (SecIdentityCopyCertificate(identity, &cert_raw) != errSecSuccess ||
        SecIdentityCopyPrivateKey(identity, &key_raw) != errSecSuccess) {
        if (cert_raw) CFRelease(cert_raw);
        if (error) *error = "the identity has no certificate or key";
        return std::nullopt;
    }
    Cf<SecCertificateRef> cert(cert_raw);
    Cf<SecKeyRef> key(key_raw);
    SigningKey k;
    k.certificate = certificate_bytes(cert.get());
    k.chain = chain.empty() ? chain_of(cert.get()) : std::move(chain);
    if (const auto f = der::certificate_fields(k.certificate)) k.team_id = f->subject_ou;
    k.sign_sha256_rsa = signer_for(key.get());
    return k;
}

} // namespace

std::optional<SigningKey> key_from_p12(const std::vector<u8>& p12, const std::string& password,
    const std::vector<std::vector<u8>>& extra_chain, std::string* error)
{
    Cf<CFDataRef> data(CFDataCreate(nullptr, p12.data(), static_cast<CFIndex>(p12.size())));
    Cf<CFStringRef> pass(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
    Cf<CFMutableDictionaryRef> options(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(options.get(), kSecImportExportPassphrase, pass.get());
#if TARGET_OS_OSX
    if (__builtin_available(macOS 15.0, *)) CFDictionarySetValue(options.get(), kSecImportToMemoryOnly, kCFBooleanTrue);
#endif
    CFArrayRef items_raw = nullptr;
    const OSStatus st = SecPKCS12Import(data.get(), options.get(), &items_raw);
    Cf<CFArrayRef> items(items_raw);
    if (st != errSecSuccess || !items || CFArrayGetCount(items.get()) == 0) {
        if (error) *error = st == errSecAuthFailed ? "wrong password for the .p12" : "the .p12 does not import";
        return std::nullopt;
    }
    const auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items.get(), 0));
    const auto identity = static_cast<SecIdentityRef>(const_cast<void*>(CFDictionaryGetValue(dict, kSecImportItemIdentity)));
    if (!identity) {
        if (error) *error = "the .p12 holds no identity";
        return std::nullopt;
    }
    std::vector<Bytes> chain;
    if (const auto certs = static_cast<CFArrayRef>(CFDictionaryGetValue(dict, kSecImportItemCertChain))) {
        for (CFIndex i = 1; i < CFArrayGetCount(certs); i++) {
            chain.push_back(certificate_bytes(static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(certs, i)))));
        }
    }
    for (const auto& c : extra_chain) {
        if (std::find(chain.begin(), chain.end(), c) == chain.end()) chain.push_back(c);
    }
    return key_from_identity(identity, std::move(chain), error);
}

std::optional<SigningKey> key_from_keychain(const std::string& name, std::string* error)
{
#if TARGET_OS_OSX
    Cf<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(query.get(), kSecClass, kSecClassIdentity);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
    CFDictionarySetValue(query.get(), kSecReturnRef, kCFBooleanTrue);
    CFTypeRef result_raw = nullptr;
    if (SecItemCopyMatching(query.get(), &result_raw) != errSecSuccess || !result_raw) {
        if (error) *error = "no signing identities in the keychains";
        return std::nullopt;
    }
    Cf<CFArrayRef> result(static_cast<CFArrayRef>(result_raw));
    for (CFIndex i = 0; i < CFArrayGetCount(result.get()); i++) {
        const auto identity = static_cast<SecIdentityRef>(const_cast<void*>(CFArrayGetValueAtIndex(result.get(), i)));
        SecCertificateRef cert_raw = nullptr;
        if (SecIdentityCopyCertificate(identity, &cert_raw) != errSecSuccess) continue;
        Cf<SecCertificateRef> cert(cert_raw);
        Cf<CFStringRef> summary(SecCertificateCopySubjectSummary(cert.get()));
        char buf[256] = {};
        if (summary) CFStringGetCString(summary.get(), buf, sizeof buf, kCFStringEncodingUTF8);
        if (std::string(buf).find(name) != std::string::npos) return key_from_identity(identity, {}, error);
    }
    if (error) *error = "no identity named like \"" + name + "\" in the keychains";
    return std::nullopt;
#else
    (void)name;
    if (error) *error = "keychain identities are looked up on macOS only";
    return std::nullopt;
#endif
}

bool export_identity(const std::string& name, const std::string& out_dir, std::string* error)
{
#if TARGET_OS_OSX
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    Cf<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(query.get(), kSecClass, kSecClassIdentity);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
    CFDictionarySetValue(query.get(), kSecReturnRef, kCFBooleanTrue);
    CFTypeRef result_raw = nullptr;
    if (SecItemCopyMatching(query.get(), &result_raw) != errSecSuccess || !result_raw) return fail("no identities");
    Cf<CFArrayRef> result(static_cast<CFArrayRef>(result_raw));
    SecIdentityRef identity = nullptr;
    Bytes leaf;
    for (CFIndex i = 0; i < CFArrayGetCount(result.get()) && !identity; i++) {
        const auto candidate = static_cast<SecIdentityRef>(const_cast<void*>(CFArrayGetValueAtIndex(result.get(), i)));
        SecCertificateRef cert_raw = nullptr;
        if (SecIdentityCopyCertificate(candidate, &cert_raw) != errSecSuccess) continue;
        Cf<SecCertificateRef> cert(cert_raw);
        Cf<CFStringRef> summary(SecCertificateCopySubjectSummary(cert.get()));
        char buf[256] = {};
        if (summary) CFStringGetCString(summary.get(), buf, sizeof buf, kCFStringEncodingUTF8);
        if (std::string(buf).find(name) == std::string::npos) continue;
        identity = candidate;
        leaf = certificate_bytes(cert.get());
    }
    if (!identity) return fail("no identity named like \"" + name + "\"");

    // A random password: 18 bytes as 24 letters and digits.
    u8 random[18];
    if (SecRandomCopyBytes(kSecRandomDefault, sizeof random, random) != errSecSuccess) return fail("no random bytes");
    static constexpr char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::string password;
    for (const u8 b : random) {
        password += kChars[b % (sizeof kChars - 1)];
        password += kChars[(b >> 4) % (sizeof kChars - 1)];
    }
    password.resize(24);
    Cf<CFStringRef> pass(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
    SecItemImportExportKeyParameters params{};
    params.version = SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION;
    params.passphrase = pass.get();
    CFDataRef p12_raw = nullptr;
    const OSStatus st = SecItemExport(identity, kSecFormatPKCS12, 0, &params, &p12_raw);
    Cf<CFDataRef> p12(p12_raw);
    if (st != errSecSuccess || !p12) return fail("the keychain did not export the identity (OSStatus " + std::to_string(st) + ")");

    const auto write = [&](const std::string& file, const u8* data, usize size) {
        const std::string path = out_dir + "/" + file;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(data, 1, size, f) == size;
        std::fclose(f);
        chmod(path.c_str(), 0600);
        return ok;
    };
    if (!write("identity.p12", CFDataGetBytePtr(p12.get()), static_cast<usize>(CFDataGetLength(p12.get()))) ||
        !write("identity.pass", reinterpret_cast<const u8*>(password.data()), password.size())) {
        return fail("cannot write into " + out_dir);
    }
    SecCertificateRef cert_raw = nullptr;
    SecIdentityCopyCertificate(identity, &cert_raw);
    Cf<SecCertificateRef> cert(cert_raw);
    const std::vector<Bytes> chain = cert ? chain_of(cert.get()) : std::vector<Bytes>{};
    for (usize i = 0; i < chain.size(); i++) {
        if (!write("chain-" + std::to_string(i + 1) + ".cer", chain[i].data(), chain[i].size())) return fail("cannot write the chain");
    }
    return true;
#else
    (void)name;
    (void)out_dir;
    if (error) *error = "identities are exported on macOS only";
    return false;
#endif
}

std::optional<SigningKey> key_for_tests(const std::string& team_id, std::string* error)
{
    // An in-memory RSA key (not in any keychain) and a self-signed certificate for it, CN "pspocket test", OU the team.
    Cf<CFMutableDictionaryRef> params(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    int bits = 2048;
    Cf<CFNumberRef> size(CFNumberCreate(nullptr, kCFNumberIntType, &bits));
    CFDictionarySetValue(params.get(), kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionarySetValue(params.get(), kSecAttrKeySizeInBits, size.get());
    CFErrorRef e = nullptr;
    Cf<SecKeyRef> key(SecKeyCreateRandomKey(params.get(), &e));
    if (!key) {
        if (error) *error = "cannot make an RSA key: " + error_text(e);
        if (e) CFRelease(e);
        return std::nullopt;
    }
    Cf<SecKeyRef> pub(SecKeyCopyPublicKey(key.get()));
    Cf<CFDataRef> pkcs1(pub ? SecKeyCopyExternalRepresentation(pub.get(), nullptr) : nullptr);
    if (!pkcs1) {
        if (error) *error = "cannot export the public key";
        return std::nullopt;
    }
    Bytes bitstring{0x00};
    const Bytes pk = bytes_of(pkcs1.get());
    bitstring.insert(bitstring.end(), pk.begin(), pk.end());
    const Bytes spki = der::seq({der::seq({der::oid_rsa(), der::null()}), der::tlv(0x03, bitstring)});
    const Bytes name = der::seq({der::set_of({der::seq({der::oid_common_name(), der::utf8("pspocket test")})}),
        der::set_of({der::seq({der::oid_ou(), der::utf8(team_id)})})});
    const std::time_t now = std::time(nullptr);
    const Bytes tbs = der::seq({der::tlv(0xA0, der::small_int(2)), der::small_int(1),
        der::seq({der::oid_sha256_rsa(), der::null()}), name,
        der::seq({der::utc_time(now - 3600), der::utc_time(now + 86400)}), name, spki});
    auto sign = signer_for(key.get());
    const auto sig = sign(tbs);
    if (!sig) {
        if (error) *error = "cannot sign the test certificate";
        return std::nullopt;
    }
    Bytes sigbits{0x00};
    sigbits.insert(sigbits.end(), sig->begin(), sig->end());
    SigningKey k;
    k.certificate = der::seq({tbs, der::seq({der::oid_sha256_rsa(), der::null()}), der::tlv(0x03, sigbits)});
    k.team_id = team_id;
    k.sign_sha256_rsa = std::move(sign);
    return k;
}

} // namespace pspocket::codesign
