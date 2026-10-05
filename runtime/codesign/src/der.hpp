#pragma once

// The ASN.1 DER the code signature's CMS needs (RFC 5652), written and read here: no ASN.1 library.

#include <algorithm>
#include <ctime>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::codesign::der {

using Bytes = std::vector<u8>;

inline Bytes tlv(u8 tag, const Bytes& content)
{
    Bytes out{tag};
    const usize n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<u8>(n));
    } else {
        u8 len[8];
        int count = 0;
        for (usize v = n; v; v >>= 8) len[count++] = static_cast<u8>(v);
        out.push_back(static_cast<u8>(0x80 | count));
        for (int i = count - 1; i >= 0; i--) out.push_back(len[i]);
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

inline Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

inline Bytes seq(std::initializer_list<Bytes> parts) { return tlv(0x30, cat(parts)); }
// A SET OF in DER order: its elements sorted by their encodings.
inline Bytes set_of(std::vector<Bytes> parts)
{
    std::sort(parts.begin(), parts.end());
    Bytes body;
    for (const Bytes& p : parts) body.insert(body.end(), p.begin(), p.end());
    return tlv(0x31, body);
}
inline Bytes octets(std::span<const u8> data) { return tlv(0x04, Bytes(data.begin(), data.end())); }
inline Bytes oid(std::initializer_list<u8> encoded) { return tlv(0x06, Bytes(encoded)); }
inline Bytes null() { return Bytes{0x05, 0x00}; }
inline Bytes small_int(u8 v) { return Bytes{0x02, 0x01, v}; }
inline Bytes utf8(std::string_view s) { return tlv(0x0C, Bytes(s.begin(), s.end())); }
// UTCTime, YYMMDDHHMMSSZ.
inline Bytes utc_time(std::time_t t)
{
    std::tm tm{};
    gmtime_r(&t, &tm);
    char s[16];
    std::strftime(s, sizeof s, "%y%m%d%H%M%SZ", &tm);
    return tlv(0x17, Bytes(s, s + 13));
}

// OIDs, encoded.
inline Bytes oid_signed_data() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02}); }
inline Bytes oid_data() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01}); }
inline Bytes oid_sha256() { return oid({0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01}); }
inline Bytes oid_rsa() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01}); }
inline Bytes oid_sha256_rsa() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B}); }
inline Bytes oid_content_type() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x03}); }
inline Bytes oid_message_digest() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04}); }
inline Bytes oid_signing_time() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x05}); }
inline Bytes oid_apple_cdhashes() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x63, 0x64, 0x09, 0x01}); }
inline Bytes oid_apple_cdhashes2() { return oid({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x63, 0x64, 0x09, 0x02}); }
inline Bytes oid_common_name() { return oid({0x55, 0x04, 0x03}); }
inline Bytes oid_ou() { return oid({0x55, 0x04, 0x0B}); }

// Reading: one element at `at` of `b`.
struct Element {
    u8 tag = 0;
    usize start = 0;   // where its tag is
    usize content = 0; // where its content starts
    usize length = 0;  // its content's length
    usize end() const { return content + length; }
};

inline std::optional<Element> read(std::span<const u8> b, usize at)
{
    if (at + 2 > b.size()) return std::nullopt;
    Element e;
    e.tag = b[at];
    e.start = at;
    usize len = b[at + 1];
    usize p = at + 2;
    if (len & 0x80) {
        const usize count = len & 0x7F;
        if (count == 0 || count > 4 || p + count > b.size()) return std::nullopt;
        len = 0;
        for (usize i = 0; i < count; i++) len = len << 8 | b[p + i];
        p += count;
    }
    if (p + len > b.size()) return std::nullopt;
    e.content = p;
    e.length = len;
    return e;
}

inline Bytes slice(std::span<const u8> b, usize from, usize to) { return Bytes(b.begin() + from, b.begin() + to); }

// What the CMS takes from a certificate: its issuer and serial number, and from its subject the common name and the
// organizational unit (Apple's development certificates carry the team ID there).
struct CertificateFields {
    Bytes issuer;        // the Name, encoded
    Bytes serial;        // the INTEGER, encoded
    std::string subject_cn;
    std::string subject_ou;
};

inline std::optional<CertificateFields> certificate_fields(std::span<const u8> cert)
{
    const auto c = read(cert, 0);                       // Certificate
    if (!c || c->tag != 0x30) return std::nullopt;
    const auto tbs = read(cert, c->content);            // TBSCertificate
    if (!tbs || tbs->tag != 0x30) return std::nullopt;
    usize at = tbs->content;
    auto e = read(cert, at);
    if (e && e->tag == 0xA0) at = e->end();             // [0] version
    const auto serial = read(cert, at);
    if (!serial || serial->tag != 0x02) return std::nullopt;
    const auto alg = read(cert, serial->end());
    if (!alg) return std::nullopt;
    const auto issuer = read(cert, alg->end());
    if (!issuer || issuer->tag != 0x30) return std::nullopt;
    const auto validity = read(cert, issuer->end());
    if (!validity) return std::nullopt;
    const auto subject = read(cert, validity->end());
    if (!subject || subject->tag != 0x30) return std::nullopt;
    CertificateFields f;
    f.issuer = slice(cert, issuer->start, issuer->end());
    f.serial = slice(cert, serial->start, serial->end());
    // Subject: SET OF { SEQUENCE { OID, value } }.
    const Bytes cn_oid = oid_common_name(), ou_oid = oid_ou();
    for (usize p = subject->content; p < subject->end();) {
        const auto rdn = read(cert, p);
        if (!rdn) break;
        for (usize q = rdn->content; q < rdn->end();) {
            const auto atv = read(cert, q);
            if (!atv) break;
            const auto type = read(cert, atv->content);
            const auto value = type ? read(cert, type->end()) : std::nullopt;
            if (type && value) {
                const Bytes t = slice(cert, type->start, type->end());
                const std::string v(cert.begin() + value->content, cert.begin() + value->end());
                if (t == cn_oid) f.subject_cn = v;
                if (t == ou_oid) f.subject_ou = v;
            }
            q = atv->end();
        }
        p = rdn->end();
    }
    return f;
}

} // namespace pspocket::codesign::der
