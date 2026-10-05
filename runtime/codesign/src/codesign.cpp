#include "pspocket/codesign/codesign.hpp"

#include <ctime>
#include <format>

#include "der.hpp"
#include "pspocket/codesign/macho.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::codesign {
namespace {

using der::Bytes;

constexpr u32 kSuperBlob = 0xFADE0CC0, kCodeDirectory = 0xFADE0C02, kRequirements = 0xFADE0C01,
              kRequirement = 0xFADE0C00, kBlobWrapper = 0xFADE0B01;
constexpr u32 kSlotCodeDirectory = 0, kSlotRequirements = 2, kSlotSignature = 0x10000;
constexpr u32 kAdhoc = 0x2, kExecSegMainBinary = 0x1, kExecute = 2;
constexpr u32 kPageShift = 12, kPage = 1u << kPageShift; // 4 KB code pages, as Apple's tools sign
constexpr u32 kSpecialSlots = 2;                          // -1 Info.plist (none), -2 requirements

void be32(Bytes& b, u32 v)
{
    for (int i = 3; i >= 0; i--) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void be64(Bytes& b, u64 v)
{
    be32(b, static_cast<u32>(v >> 32));
    be32(b, static_cast<u32>(v));
}
void put32(Bytes& b, usize at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<u8>(v >> (8 * i)); // little-endian Mach-O fields
}
void put64(Bytes& b, usize at, u64 v)
{
    put32(b, at, static_cast<u32>(v));
    put32(b, at + 4, static_cast<u32>(v >> 32));
}
u32 get_be32(const Bytes& b, usize at)
{
    return static_cast<u32>(b[at]) << 24 | static_cast<u32>(b[at + 1]) << 16 | static_cast<u32>(b[at + 2]) << 8 | b[at + 3];
}
Bytes sha256(std::span<const u8> data)
{
    const auto d = recomp::Sha256::hash(data);
    return Bytes(d.begin(), d.end());
}

std::string base64(std::span<const u8> data)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < data.size(); i += 3) {
        const u32 n = static_cast<u32>(data[i]) << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) |
            (i + 2 < data.size() ? data[i + 2] : 0);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < data.size() ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

// The requirement language, compiled (Apple's reqmaker): ops and data as big-endian words, data padded to 4 bytes.
void req_data(Bytes& b, std::string_view s)
{
    be32(b, static_cast<u32>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
    while (b.size() % 4) b.push_back(0);
}

// The requirements blob. With an identity, the designated requirement codesign and zsign write for a development
// certificate: identifier "<id>" and anchor apple generic and certificate leaf[subject.CN] = "<CN>" and
// certificate 1[field.1.2.840.113635.100.6.2.1] exists. Ad hoc: an empty set, as codesign -s - writes.
Bytes requirements(const std::string& identifier, const std::string& leaf_cn)
{
    Bytes b;
    if (leaf_cn.empty()) {
        be32(b, kRequirements);
        be32(b, 12);
        be32(b, 0);
        return b;
    }
    constexpr u32 kAnd = 6, kIdent = 2, kAppleGenericAnchor = 15, kCertField = 11, kCertGeneric = 14;
    constexpr u32 kMatchExists = 0, kMatchEqual = 1;
    Bytes expr;
    be32(expr, kAnd);
    be32(expr, kAnd);
    be32(expr, kAnd);
    be32(expr, kIdent);
    req_data(expr, identifier);
    be32(expr, kAppleGenericAnchor);
    be32(expr, kCertField);
    be32(expr, 0); // leaf
    req_data(expr, "subject.CN");
    be32(expr, kMatchEqual);
    req_data(expr, leaf_cn);
    be32(expr, kCertGeneric);
    be32(expr, 1); // the intermediate: Apple's WWDR marker extension
    static constexpr u8 kWwdrMarker[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x63, 0x64, 0x06, 0x02, 0x01};
    req_data(expr, std::string_view(reinterpret_cast<const char*>(kWwdrMarker), sizeof kWwdrMarker));
    be32(expr, kMatchExists);
    Bytes req;
    be32(req, kRequirement);
    be32(req, static_cast<u32>(12 + expr.size()));
    be32(req, 1); // expression form
    req.insert(req.end(), expr.begin(), expr.end());
    be32(b, kRequirements);
    be32(b, static_cast<u32>(20 + req.size()));
    be32(b, 1);
    be32(b, 3); // designated requirement
    be32(b, 20);
    b.insert(b.end(), req.begin(), req.end());
    return b;
}

// The CodeDirectory, version 0x20400, over [0, code_limit) of `file`.
Bytes code_directory(const Bytes& file, u32 code_limit, const std::string& identifier, const std::string& team,
    const Bytes& requirements_blob, const MachOLayout& l, bool adhoc)
{
    const u32 code_slots = (code_limit + kPage - 1) / kPage;
    const u32 ident_offset = 88;
    const u32 team_offset = team.empty() ? 0 : ident_offset + static_cast<u32>(identifier.size()) + 1;
    const u32 strings_end = ident_offset + static_cast<u32>(identifier.size()) + 1 +
        (team.empty() ? 0 : static_cast<u32>(team.size()) + 1);
    const u32 hash_offset = strings_end + kSpecialSlots * 32;
    const u32 length = hash_offset + code_slots * 32;
    Bytes cd;
    be32(cd, kCodeDirectory);
    be32(cd, length);
    be32(cd, 0x20400);
    be32(cd, adhoc ? kAdhoc : 0);
    be32(cd, hash_offset);
    be32(cd, ident_offset);
    be32(cd, kSpecialSlots);
    be32(cd, code_slots);
    be32(cd, code_limit);
    cd.push_back(32);        // hash size
    cd.push_back(2);         // SHA-256
    cd.push_back(0);         // platform
    cd.push_back(kPageShift); // page size
    be32(cd, 0);             // spare2
    be32(cd, 0);             // scatter
    be32(cd, team_offset);
    be32(cd, 0);  // spare3
    be64(cd, 0);  // codeLimit64
    be64(cd, l.text_fileoff);
    be64(cd, l.text_filesize);
    be64(cd, l.filetype == kExecute ? kExecSegMainBinary : 0);
    cd.insert(cd.end(), identifier.begin(), identifier.end());
    cd.push_back(0);
    if (!team.empty()) {
        cd.insert(cd.end(), team.begin(), team.end());
        cd.push_back(0);
    }
    // Special slots, -2 first: the requirements, then the (absent) Info.plist.
    const Bytes req_hash = sha256(requirements_blob);
    cd.insert(cd.end(), req_hash.begin(), req_hash.end());
    cd.insert(cd.end(), 32, 0);
    for (u32 i = 0; i < code_slots; i++) {
        const usize from = usize{i} * kPage, to = std::min<usize>(from + kPage, code_limit);
        const Bytes h = sha256(std::span<const u8>(file.data() + from, to - from));
        cd.insert(cd.end(), h.begin(), h.end());
    }
    return cd;
}

std::string cdhash_plist(std::span<const u8> cdhash20)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n\t<key>cdhashes</key>\n\t<array>\n\t\t<data>\n\t\t" +
        base64(cdhash20) + "\n\t\t</data>\n\t</array>\n</dict>\n</plist>\n";
}

// The detached CMS SignedData over the CodeDirectory, as Apple's codesign makes it (content type data, the signer's
// certificate and chain, the CDHashes attributes).
std::optional<Bytes> cms(const Bytes& cd, const SigningKey& key, std::time_t when, std::string* error)
{
    const auto fields = der::certificate_fields(key.certificate);
    if (!fields) {
        if (error) *error = "cannot read the signing certificate";
        return std::nullopt;
    }
    const Bytes cdhash = sha256(cd);
    const std::string plist = cdhash_plist(std::span<const u8>(cdhash.data(), 20));
    const std::vector<Bytes> attrs = {
        der::seq({der::oid_content_type(), der::set_of({der::oid_data()})}),
        der::seq({der::oid_signing_time(), der::set_of({der::utc_time(when)})}),
        der::seq({der::oid_message_digest(), der::set_of({der::octets(cdhash)})}),
        der::seq({der::oid_apple_cdhashes(), der::set_of({der::octets(std::span<const u8>(
            reinterpret_cast<const u8*>(plist.data()), plist.size()))})}),
        der::seq({der::oid_apple_cdhashes2(), der::set_of({der::seq({der::oid_sha256(), der::octets(cdhash)})})}),
    };
    Bytes signed_attrs = der::set_of(attrs); // signed with the SET tag (RFC 5652 5.4) ...
    const auto signature = key.sign_sha256_rsa(signed_attrs);
    if (!signature) {
        if (error) *error = "the signing key refused to sign";
        return std::nullopt;
    }
    signed_attrs[0] = 0xA0; // ... and carried as [0] IMPLICIT
    const Bytes signer = der::seq({der::small_int(1), der::seq({fields->issuer, fields->serial}),
        der::seq({der::oid_sha256()}), signed_attrs, der::seq({der::oid_rsa(), der::null()}), der::octets(*signature)});
    Bytes certs = key.certificate;
    for (const Bytes& c : key.chain) certs.insert(certs.end(), c.begin(), c.end());
    const Bytes signed_data = der::seq({der::small_int(1), der::set_of({der::seq({der::oid_sha256()})}),
        der::seq({der::oid_data()}), der::tlv(0xA0, certs), der::set_of({signer})});
    return der::seq({der::oid_signed_data(), der::tlv(0xA0, signed_data)});
}

} // namespace

bool sign(std::vector<u8>& file, const SignOptions& options, std::string* error)
{
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    std::string why;
    const auto layout = read_layout(file, &why);
    if (!layout) return fail(why);
    if (options.identifier.empty()) return fail("a code identifier is needed");
    const MachOLayout& l = *layout;
    const bool adhoc = options.key == nullptr;
    std::string team, leaf_cn;
    if (!adhoc) {
        const auto fields = der::certificate_fields(options.key->certificate);
        if (!fields) return fail("cannot read the signing certificate");
        team = !options.key->team_id.empty() ? options.key->team_id : fields->subject_ou;
        leaf_cn = fields->subject_cn;
        if (team.empty()) return fail("the certificate names no team (its OU)");
    }
    const u32 code_limit = l.signature_offset;
    const Bytes req = requirements(options.identifier, leaf_cn);

    // The space the signature takes, fixed before the pages are hashed (the header's sizes are in the first page).
    const u32 code_slots = (code_limit + kPage - 1) / kPage;
    usize estimate = 12 + 3 * 8 + 88 + options.identifier.size() + team.size() + 2 + (kSpecialSlots + code_slots) * 32 +
        req.size() + 8;
    if (!adhoc) {
        estimate += options.key->certificate.size() + 4096; // the CMS: certificates, attributes, a 4096-bit signature
        for (const Bytes& c : options.key->chain) estimate += c.size();
    }
    const u32 data_size = static_cast<u32>((estimate + 15) & ~usize{15});
    file.resize(code_limit);
    file.resize(code_limit + usize{data_size}, 0);
    put32(file, l.code_signature_cmd + 12, data_size);
    const u64 linkedit_size = u64{code_limit} + data_size - l.linkedit_fileoff;
    put64(file, l.linkedit_cmd + 32, (linkedit_size + 0x3FFF) & ~u64{0x3FFF}); // vmsize, in 16 KB pages
    put64(file, l.linkedit_cmd + 48, linkedit_size);                          // filesize

    const Bytes cd = code_directory(file, code_limit, options.identifier, team, req, l, adhoc);
    Bytes signature_blob;
    be32(signature_blob, kBlobWrapper);
    if (adhoc) {
        be32(signature_blob, 8);
    } else {
        const std::time_t when = options.signing_time ? static_cast<std::time_t>(options.signing_time) : std::time(nullptr);
        const auto c = cms(cd, *options.key, when, &why);
        if (!c) return fail(why);
        be32(signature_blob, static_cast<u32>(8 + c->size()));
        signature_blob.insert(signature_blob.end(), c->begin(), c->end());
    }
    Bytes blob;
    const u32 header = 12 + 3 * 8;
    be32(blob, kSuperBlob);
    be32(blob, static_cast<u32>(header + cd.size() + req.size() + signature_blob.size()));
    be32(blob, 3);
    be32(blob, kSlotCodeDirectory);
    be32(blob, header);
    be32(blob, kSlotRequirements);
    be32(blob, static_cast<u32>(header + cd.size()));
    be32(blob, kSlotSignature);
    be32(blob, static_cast<u32>(header + cd.size() + req.size()));
    blob.insert(blob.end(), cd.begin(), cd.end());
    blob.insert(blob.end(), req.begin(), req.end());
    blob.insert(blob.end(), signature_blob.begin(), signature_blob.end());
    if (blob.size() > data_size) return fail(std::format("the signature ({} bytes) outgrew its space ({})", blob.size(), data_size));
    std::copy(blob.begin(), blob.end(), file.begin() + code_limit);
    return true;
}

std::optional<std::vector<u8>> code_directory_hash(const std::vector<u8>& file)
{
    const auto l = read_layout(file);
    if (!l || l->signature_size < 12 || u64{l->signature_offset} + l->signature_size > file.size()) return std::nullopt;
    const usize base = l->signature_offset;
    if (get_be32(file, base) != kSuperBlob) return std::nullopt;
    const u32 count = get_be32(file, base + 8);
    for (u32 i = 0; i < count; i++) {
        const usize entry = base + 12 + usize{i} * 8;
        if (entry + 8 > file.size()) return std::nullopt;
        if (get_be32(file, entry) != kSlotCodeDirectory) continue;
        const usize cd = base + get_be32(file, entry + 4);
        if (cd + 8 > file.size() || get_be32(file, cd) != kCodeDirectory) return std::nullopt;
        const usize length = get_be32(file, cd + 4);
        if (cd + length > file.size()) return std::nullopt;
        return sha256(std::span<const u8>(file.data() + cd, length));
    }
    return std::nullopt;
}

} // namespace pspocket::codesign
