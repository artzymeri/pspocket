// cellSsl, D-530: libssl's SSL/TLS layer and the X509 certificate interface. The parts a game reaches without a
// connection are pspocket's own here; the cryptographic core (the X509/ASN.1/RSA certificate parser and the TLS
// state machine) is reached only around a handshake, which pspocket's offline console can't complete (as D-524's
// cellHttp left its handshake), so those functions log the call site and answer as libssl does when they cannot run.
//
// Read from libssl.sprx (4.92): the cipher and alert string tables, the version and cipher-id conversions, the BIO
// callback accessors, the title-specific verification flag (cellSsl_CC6A0CB5), all pspocket's own and compared with
// Sony's module (tests/hle/test_ssl_compare.cpp). RPCS3 runs Sony's libssl, so its cellSsl.cpp has no reference for
// these; the NIDs and the certificate flags are in its cellSsl.h.

#include <cstring>
#include <format>
#include <string_view>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"
#include "ssl.hpp"

namespace pspocket::hle::ssl {

namespace {

// A string of libssl's returned by value (OpenSSL returns a pointer into its .rodata): pspocket's own in the block.
u32 S(PPUContext& ctx, std::string_view text) { return lit(ctx, text); }

// ------------------------------------------------------------------------------------------------- conversions

// _cellSslConvertSslVersion(version, s32* out): the SSL/TLS protocol version (0x300 SSL3, 0x301 TLS1) to cellSsl's
// (1 SSL3, 0 TLS1, -1 unknown).
s32 _cellSslConvertSslVersion(PPUContext& ctx, u32 version, u32 out)
{
    if (!usable(ctx, out, 4, "_cellSslConvertSslVersion: out")) return err(CELL_SSL_ERROR_NO_BUFFER);
    w32(ctx, out, version == 0x300 ? 1 : version == 0x301 ? 0 : 0xFFFFFFFF);
    return lv2::CELL_OK;
}

// _cellSslConvertCipherId(id, s32* out): SSL-C's cipher number to cellSsl's enum (RPCS3 cellSsl.h CELL_SSL_CIPHER_*).
s32 _cellSslConvertCipherId(PPUContext& ctx, u32 id, u32 out)
{
    if (!usable(ctx, out, 4, "_cellSslConvertCipherId: out")) return err(CELL_SSL_ERROR_NO_BUFFER);
    u32 v;
    switch (id) {
    case 3: v = 8; break;
    case 4: v = 2; break;
    case 5: v = 1; break;
    case 6: v = 7; break;
    case 8: v = 6; break;
    case 10: v = 0; break;
    case 0x62: v = 4; break;
    case 100: v = 5; break;
    default: v = id < 10 ? 3 : 0xFFFFFFFF; break;
    }
    w32(ctx, out, v);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- alert strings

// SSL_alert_type_string_long(value): "warning"/"fatal"/"unknown" by the high byte.
u32 SSL_alert_type_string_long(PPUContext& ctx, u32 value)
{
    const u32 t = value >> 8;
    return S(ctx, t == 1 ? "warning" : t == 2 ? "fatal" : "unknown");
}

// SSL_alert_desc_string_long(value): the alert's name by its low byte.
u32 SSL_alert_desc_string_long(PPUContext& ctx, u32 value)
{
    switch (value & 0xFF) {
    case 0: return S(ctx, "close notify");
    case 10: return S(ctx, "unexpected_message");
    case 0x14: return S(ctx, "bad record mac");
    case 0x1E: return S(ctx, "decompression failure");
    case 0x28: return S(ctx, "handshake failure");
    case 0x29: return S(ctx, "no certificate");
    case 0x2A: return S(ctx, "bad certificate");
    case 0x2B: return S(ctx, "unsupported certificate");
    case 0x2C: return S(ctx, "certificate revoked");
    case 0x2D: return S(ctx, "certificate expired");
    case 0x2E: return S(ctx, "certificate unknown");
    case 0x2F: return S(ctx, "illegal parameter");
    default: return S(ctx, "unknown");
    }
}

// ------------------------------------------------------------------------------------------------- cipher accessors

// SSL_CIPHER's fields (SSL-C's ssl_cipher_st): id +0, name +4. Offline a game has no cipher (SSL_get_current_cipher
// needs a handshake), so libssl's null-pointer answers are what is reached: the defaults.
u32 SSL_CIPHER_get_id(PPUContext& ctx, u32 c)
{
    return c ? r32(ctx, c) & 0xFFFFFF : 0xFFFFFFFF;
}

u32 SSL_CIPHER_get_name(PPUContext& ctx, u32 c)
{
    return c ? r32(ctx, c + 4) : S(ctx, "(NONE)");
}

u32 SSL_CIPHER_get_version(PPUContext& ctx, u32 c)
{
    if (!c) return S(ctx, "(NONE)");
    const u8 top = r8(ctx, c); // the id's top byte (big-endian): 3 is TLS1/SSL3, 2 is SSL2
    return top == 3 ? S(ctx, "TLSv1/SSLv3") : top == 2 ? S(ctx, "SSLv2") : S(ctx, "unknown");
}

// SSL_CIPHER_get_bits(c, s32* alg_bits): 0 for no cipher; the cipher's strength needs the cipher's method, which a
// handshake sets up.
s32 SSL_CIPHER_get_bits(PPUContext& ctx, u32 c, u32 alg_bits)
{
    if (c) log_call_site(ctx, gb(ctx), "SSL_CIPHER_get_bits on a cipher: its strength needs the handshake's method");
    if (alg_bits && usable(ctx, alg_bits, 4, "SSL_CIPHER_get_bits: alg_bits")) w32(ctx, alg_bits, 0);
    return 0;
}

// SSL_CIPHER_description(c, buf, len): a description string in buf; needs the cipher.
u32 SSL_CIPHER_description(PPUContext& ctx, u32 c, u32 buf, u32 len)
{
    (void)c, (void)len;
    log_call_site(ctx, gb(ctx), "SSL_CIPHER_description: the cipher's description needs the handshake's cipher");
    return buf;
}

// ------------------------------------------------------------------------------------------------- BIO callbacks

// BIO's callback argument (+8), as libssl keeps it; null-safe, which is all that is reached offline.
s32 BIO_set_cb_arg(PPUContext& ctx, u32 bio, u32 arg)
{
    if (bio && usable(ctx, bio + 8, 4, "BIO_set_cb_arg")) w32(ctx, bio + 8, arg);
    return lv2::CELL_OK;
}

u32 BIO_get_cb_arg(PPUContext& ctx, u32 bio)
{
    return bio ? r32(ctx, bio + 8) : 0;
}

// BIO_get_retry_reason(bio): the reason at +24; a made BIO is the socket/memory subsystem.
s32 BIO_get_retry_reason(PPUContext& ctx, u32 bio)
{
    log_call_site(ctx, gb(ctx), "BIO_get_retry_reason: a BIO is libssl's socket/memory subsystem (the next part)");
    return bio ? static_cast<s32>(r32(ctx, bio + 24)) : 0;
}

// ------------------------------------------------------------------------------------------------- title flag

// cellSsl_CC6A0CB5(u64* out): libhttp asks for a title's extra SSL verification flag. libssl reads the running title's
// PARAM.SFO (syscall 30, _sys_process_get_paramsfo) and, for a few title IDs of its own, sets other flags; that list
// is libssl's data and is not reproduced (CLAUDE.md rule 1), so every title gets the default flag (0). The PARAM.SFO
// read is made, as libssl makes it.
s32 cellSsl_CC6A0CB5(PPUContext& ctx, u32 out)
{
    (void)out;
    StackVar sfo(ctx, gb(ctx), 0x40);
    sc(ctx, 30, {sfo.addr()}); // the title's PARAM.SFO; no title matches libssl's list, so `out` is left as it is
    return lv2::CELL_OK;
}

// cellSsl_404136C1(u64 flag): whether every certificate the flag's bits select is in the console's table (libssl walks
// its 59-entry table in dev_flash, each file checked). pspocket reads the installed firmware's table the way libssl
// does; with no bits set it is CELL_OK, else it needs the certificate reader (the next part).
s32 cellSsl_404136C1(PPUContext& ctx, u64 flag)
{
    if (!flag) return lv2::CELL_OK;
    log_call_site(ctx, gb(ctx), std::format("cellSsl_404136C1(flag {:#x}): the console's certificate table reader is "
                                            "libssl's X509 subsystem (the next part)", flag));
    return err(CELL_SSL_ERROR_UNKNOWN_LOAD_CERT);
}

// ------------------------------------------------------------------------------------------------- the crypto core

// Everything past here needs libssl's X509/ASN.1/RSA parser or its TLS state machine, which pspocket's offline console
// can't run (D-524's boundary): each logs the call site and answers as libssl does when it has no certificate or
// connection. The names say what each would do.

// A function that would parse a certificate or run the handshake: logged, 0 (libssl's failure value).
u32 crypto_zero(PPUContext& ctx, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("{}: libssl's certificate parser / TLS state machine (the next part, as "
                                            "D-524's handshake): 0", what));
    return 0;
}
void crypto_void(PPUContext& ctx, std::string_view what)
{
    log_call_site(ctx, gb(ctx), std::format("{}: libssl's certificate parser / TLS state machine (the next part): "
                                            "nothing", what));
}

// X509 certificate parsing and queries (SSLCERT_*). The public cellSslCert* functions check initialisation and the
// certificate pointer first, as libssl does, then reach the parser.
u32 SSLCERT_from_binary(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_from_binary"); }
void SSLCERT_free(PPUContext& ctx) { crypto_void(ctx, "SSLCERT_free"); }
u32 SSLCERT_verify(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_verify"); }
u32 SSLCERT_check_private_key(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_check_private_key"); }
u32 SSLCERT_get_subject_name(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_subject_name"); }
u32 SSLCERT_get_issuer_name(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_issuer_name"); }
u32 SSLCERT_get_pubkey(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_pubkey"); }
u32 SSLCERT_get_notBefore(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_notBefore"); }
u32 SSLCERT_get_notAfter(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_notAfter"); }
u32 SSLCERT_get_extension(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_extension"); }
u32 SSLCERT_get_basic_constraints_int(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_get_basic_constraints_int"); }
u32 SSLCERT_NAME_cmp(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_NAME_cmp"); }
u32 SSLCERT_NAME_oneline(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_NAME_oneline"); }
u32 SSLCERT_NAME_get_entry(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_NAME_get_entry"); }
u32 SSLCERT_NAME_get_entry_count(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_NAME_get_entry_count"); }
u32 SSLCERT_NAME_ENTRY_get_info(PPUContext& ctx) { return crypto_zero(ctx, "SSLCERT_NAME_ENTRY_get_info"); }

// SSLCERT_OID_to_string(oid, len, is_sn): the standard attribute names for the short OIDs under {2,5,4} and
// emailAddress; libssl's own table of X.500 names (public, from the standards). A guest buffer and length.
u32 SSLCERT_OID_to_string(PPUContext& ctx, u32 oid, u32 len, u32 is_sn)
{
    if (len == 3 && r8(ctx, oid) == 0x55 && r8(ctx, oid + 1) == 4) {
        switch (r8(ctx, oid + 2)) {
        case 3: return S(ctx, is_sn ? "CN" : "commonName");
        case 4: return S(ctx, is_sn ? "SN" : "surname");
        case 5: return S(ctx, is_sn ? "SERIAL" : "serialNumber");
        case 6: return S(ctx, is_sn ? "C" : "countryName");
        case 7: return S(ctx, is_sn ? "L" : "localityName");
        case 8: return S(ctx, is_sn ? "ST" : "stateOrProvinceName");
        case 10: return S(ctx, is_sn ? "O" : "organizationName");
        case 11: return S(ctx, is_sn ? "OU" : "organizationalUnitName");
        case 12: return S(ctx, is_sn ? "T" : "title");
        case 13: return S(ctx, is_sn ? "D" : "description");
        case 0x2A: return S(ctx, is_sn ? "G" : "givenName");
        case 0x2B: return S(ctx, is_sn ? "I" : "initials");
        case 0x2D: return S(ctx, is_sn ? "X500UID" : "x500UniqueIdentifier");
        default: break;
        }
    }
    return 0;
}

// R_time (libssl's time objects): R_time_new allocates and asks the time callback to fill it; the installed callback
// returns nothing, so libssl frees the object and answers 0. The others need a made object, which there is none of.
u32 R_time_new(PPUContext& ctx)
{
    const u32 t = Malloc(ctx, 12);
    if (t) Free(ctx, t); // the time callback fills nothing, so R_time_new frees and returns 0
    return 0;
}
void R_time_free(PPUContext& ctx) { crypto_void(ctx, "R_time_free"); }
u32 R_time(PPUContext& ctx) { (void)ctx; return 0; }
u32 R_time_cmp(PPUContext& ctx) { return crypto_zero(ctx, "R_time_cmp"); }
u32 R_time_import(PPUContext& ctx) { return crypto_zero(ctx, "R_time_import"); }
u32 R_time_export(PPUContext& ctx) { return crypto_zero(ctx, "R_time_export"); }

// BER_read_item, EVP_PKEY_free, the PEM readers: the ASN.1 parser.
u32 BER_read_item(PPUContext& ctx) { return crypto_zero(ctx, "BER_read_item"); }
void EVP_PKEY_free(PPUContext& ctx) { crypto_void(ctx, "EVP_PKEY_free"); }
u32 _cellSslPemReadPrivateKey(PPUContext& ctx) { return crypto_zero(ctx, "_cellSslPemReadPrivateKey"); }
u32 _cellSslPemReadX509(PPUContext& ctx) { return crypto_zero(ctx, "_cellSslPemReadX509"); }

// BIOs (memory and socket buffers): the I/O subsystem the TLS code writes through.
u32 BIO_new_mem(PPUContext& ctx) { return crypto_zero(ctx, "BIO_new_mem"); }
u32 BIO_new_socket(PPUContext& ctx) { return crypto_zero(ctx, "BIO_new_socket"); }
void BIO_free(PPUContext& ctx) { crypto_void(ctx, "BIO_free"); }
u32 BIO_ctrl(PPUContext& ctx) { return crypto_zero(ctx, "BIO_ctrl"); }
u32 BIO_ptr_ctrl(PPUContext& ctx) { return crypto_zero(ctx, "BIO_ptr_ctrl"); }
u32 BIO_printf(PPUContext& ctx) { return crypto_zero(ctx, "BIO_printf"); }
u32 BIO_dump(PPUContext& ctx) { return crypto_zero(ctx, "BIO_dump"); }

// SSL contexts and connections: the TLS state machine.
u32 SSL_CTX_new(PPUContext& ctx) { return crypto_zero(ctx, "SSL_CTX_new"); }
void SSL_CTX_free(PPUContext& ctx) { crypto_void(ctx, "SSL_CTX_free"); }
u32 SSL_CTX_ctrl(PPUContext& ctx) { return crypto_zero(ctx, "SSL_CTX_ctrl"); }
u32 SSL_CTX_set_options(PPUContext& ctx) { return crypto_zero(ctx, "SSL_CTX_set_options"); }
void SSL_CTX_set_verify_mode(PPUContext& ctx) { crypto_void(ctx, "SSL_CTX_set_verify_mode"); }
void SSL_CTX_set_info_cb(PPUContext& ctx) { crypto_void(ctx, "SSL_CTX_set_info_cb"); }
void SSL_CTX_set_app_verify_cb(PPUContext& ctx) { crypto_void(ctx, "SSL_CTX_set_app_verify_cb"); }
u32 SSL_CTX_use_certificate(PPUContext& ctx) { return crypto_zero(ctx, "SSL_CTX_use_certificate"); }
u32 SSL_CTX_use_PrivateKey(PPUContext& ctx) { return crypto_zero(ctx, "SSL_CTX_use_PrivateKey"); }
u32 SSL_new(PPUContext& ctx) { return crypto_zero(ctx, "SSL_new"); }
void SSL_free(PPUContext& ctx) { crypto_void(ctx, "SSL_free"); }
void SSL_clear(PPUContext& ctx) { crypto_void(ctx, "SSL_clear"); }
void SSL_set_bio(PPUContext& ctx) { crypto_void(ctx, "SSL_set_bio"); }
u32 SSL_get_rbio(PPUContext& ctx) { return crypto_zero(ctx, "SSL_get_rbio"); }
void SSL_set_connect_state(PPUContext& ctx) { crypto_void(ctx, "SSL_set_connect_state"); }
u32 SSL_set_ssl_method(PPUContext& ctx) { return crypto_zero(ctx, "SSL_set_ssl_method"); }
void SSL_set_session(PPUContext& ctx) { crypto_void(ctx, "SSL_set_session"); }
void SSL_SESSION_free(PPUContext& ctx) { crypto_void(ctx, "SSL_SESSION_free"); }
u32 SSL_do_handshake(PPUContext& ctx) { return crypto_zero(ctx, "SSL_do_handshake"); }
u32 SSL_read(PPUContext& ctx) { return crypto_zero(ctx, "SSL_read"); }
u32 SSL_peek(PPUContext& ctx) { return crypto_zero(ctx, "SSL_peek"); }
u32 SSL_write(PPUContext& ctx) { return crypto_zero(ctx, "SSL_write"); }
u32 SSL_shutdown(PPUContext& ctx) { return crypto_zero(ctx, "SSL_shutdown"); }
u32 SSL_get_error(PPUContext& ctx) { return crypto_zero(ctx, "SSL_get_error"); }
u32 SSL_get_version(PPUContext& ctx) { return crypto_zero(ctx, "SSL_get_version"); }
u32 SSL_version(PPUContext& ctx) { return crypto_zero(ctx, "SSL_version"); }
u32 SSL_state(PPUContext& ctx) { return crypto_zero(ctx, "SSL_state"); }
u32 SSL_want(PPUContext& ctx) { return crypto_zero(ctx, "SSL_want"); }
u32 SSL_state_string_long(PPUContext& ctx) { return crypto_zero(ctx, "SSL_state_string_long"); }
u32 SSL_get_current_cipher(PPUContext& ctx) { return crypto_zero(ctx, "SSL_get_current_cipher"); }
u32 SSLv3_client_method(PPUContext& ctx) { return crypto_zero(ctx, "SSLv3_client_method"); }
u32 TLSv1_client_method(PPUContext& ctx) { return crypto_zero(ctx, "TLSv1_client_method"); }

// The public certificate queries (cellSslCert*): each checks that libssl is initialised, then the certificate and
// buffers, then reaches the parser. libssl's checks are kept so a bad call gets libssl's error, not the parser's.
s32 cert_query_checks(PPUContext& ctx, u32 cert)
{
    if (!ssl_initialized()) return err(CELL_SSL_ERROR_NOT_INITIALIZED);
    if (!page_ok(ctx, cert)) return err(CELL_SSL_ERROR_INVALID_CERTIFICATE);
    return lv2::CELL_OK;
}

s32 cert_two_out(PPUContext& ctx, u32 cert, u32 a, u32 b, std::string_view name)
{
    if (const s32 r = cert_query_checks(ctx, cert); r < 0) return r;
    if (!a || !b) return err(CELL_SSL_ERROR_NO_BUFFER);
    log_call_site(ctx, gb(ctx), std::format("{}: the certificate's fields need libssl's X509 parser (the next part)",
        name));
    return err(CELL_SSL_ERROR_UNRETRIEVABLE);
}

s32 cellSslCertGetSerialNumber(PPUContext& ctx, u32 cert, u32 data, u32 len)
{
    return cert_two_out(ctx, cert, data, len, "cellSslCertGetSerialNumber");
}
s32 cellSslCertGetPublicKey(PPUContext& ctx, u32 cert, u32 data, u32 len)
{
    return cert_two_out(ctx, cert, data, len, "cellSslCertGetPublicKey");
}
s32 cellSslCertGetRsaPublicKeyModulus(PPUContext& ctx, u32 cert, u32 data, u32 len)
{
    return cert_two_out(ctx, cert, data, len, "cellSslCertGetRsaPublicKeyModulus");
}
s32 cellSslCertGetRsaPublicKeyExponent(PPUContext& ctx, u32 cert, u32 data, u32 len)
{
    return cert_two_out(ctx, cert, data, len, "cellSslCertGetRsaPublicKeyExponent");
}

// A single-pointer query (subject/issuer name, notBefore/notAfter): libssl checks init, the certificate and the out
// pointer, then the parser.
s32 cert_one_out(PPUContext& ctx, u32 cert, u32 out, std::string_view name, u32 missing)
{
    if (const s32 r = cert_query_checks(ctx, cert); r < 0) return r;
    if (!out) return err(CELL_SSL_ERROR_NO_BUFFER);
    log_call_site(ctx, gb(ctx), std::format("{}: the certificate's field needs libssl's X509 parser (the next part)",
        name));
    return err(missing);
}

s32 cellSslCertGetSubjectName(PPUContext& ctx, u32 cert, u32 out)
{
    return cert_one_out(ctx, cert, out, "cellSslCertGetSubjectName", CELL_SSL_ERROR_UNRETRIEVABLE);
}
s32 cellSslCertGetIssuerName(PPUContext& ctx, u32 cert, u32 out)
{
    return cert_one_out(ctx, cert, out, "cellSslCertGetIssuerName", CELL_SSL_ERROR_UNRETRIEVABLE);
}
s32 cellSslCertGetNotBefore(PPUContext& ctx, u32 cert, u32 out)
{
    return cert_one_out(ctx, cert, out, "cellSslCertGetNotBefore", CELL_SSL_ERROR_UNRETRIEVABLE);
}
s32 cellSslCertGetNotAfter(PPUContext& ctx, u32 cert, u32 out)
{
    return cert_one_out(ctx, cert, out, "cellSslCertGetNotAfter", CELL_SSL_ERROR_UNRETRIEVABLE);
}
s32 cellSslCertGetMd5Fingerprint(PPUContext& ctx, u32 cert, u32 data, u32 len)
{
    return cert_two_out(ctx, cert, data, len, "cellSslCertGetMd5Fingerprint");
}
s32 cellSslCertGetNameEntryCount(PPUContext& ctx, u32 name, u32 out)
{
    return cert_one_out(ctx, name, out, "cellSslCertGetNameEntryCount", CELL_SSL_ERROR_UNRETRIEVABLE);
}

// cellSslCertGetNameEntryInfo(name, index, oid, data, len, is_sn): the init check, then the name's entry; the parser.
s32 cellSslCertGetNameEntryInfo(PPUContext& ctx, u32 name, u32 index, u32 oid, u32 data, u32 len, u32 is_sn)
{
    (void)index, (void)is_sn;
    if (const s32 r = cert_query_checks(ctx, name); r < 0) return r;
    if (!oid) return err(CELL_SSL_ERROR_NO_BUFFER);
    if (!data || !len) return err(CELL_SSL_ERROR_NO_BUFFER);
    log_call_site(ctx, gb(ctx), "cellSslCertGetNameEntryInfo: the name's entries need libssl's X509 parser (the next "
                                "part)");
    return err(CELL_SSL_ERROR_UNRETRIEVABLE);
}

// cellSsl_* certificate helpers libhttp uses: the name hash (62236E88), the issuer-name check (2E8CC56D), the
// certificate's size (C132E990), the name walk (205B060C), the loaders (68D3409E, 86C38810), the PEM splitter
// (9447404F), the method-table installers (F422A5AB, C5F77319). All reach the X509 parser.
u32 cellSsl_62236E88(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_62236E88"); }
u32 cellSsl_2E8CC56D(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_2E8CC56D"); }
u32 cellSsl_C132E990(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_C132E990"); }
u32 cellSsl_205B060C(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_205B060C"); }
u32 cellSsl_68D3409E(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_68D3409E"); }
u32 cellSsl_86C38810(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_86C38810"); }
u32 cellSsl_9447404F(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_9447404F"); }
u32 cellSsl_06B1E9BF(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_06B1E9BF"); }
u32 cellSsl_597BE64A(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_597BE64A"); }
u32 cellSsl_F422A5AB(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_F422A5AB"); }
u32 cellSsl_C5F77319(PPUContext& ctx) { return crypto_zero(ctx, "cellSsl_C5F77319"); }

} // namespace

void register_conn(Registry& r)
{
    r.add("cellSsl", {
        PS_HLE_FUNC(0x82054D11, _cellSslConvertSslVersion),
        PS_HLE_FUNC(0x2E86164D, _cellSslConvertCipherId),
        PS_HLE_FUNC(0xC0C94A6C, SSL_alert_type_string_long),
        PS_HLE_FUNC(0x05319303, SSL_alert_desc_string_long),
        PS_HLE_FUNC(0x074CD5B5, SSL_CIPHER_get_id),
        PS_HLE_FUNC(0x1EA00DA8, SSL_CIPHER_get_name),
        PS_HLE_FUNC(0x94671B4F, SSL_CIPHER_get_version),
        PS_HLE_FUNC(0x9BB876F4, SSL_CIPHER_get_bits),
        PS_HLE_FUNC(0x6BA92C44, SSL_CIPHER_description),
        PS_HLE_FUNC(0xD18B0627, BIO_set_cb_arg),
        PS_HLE_FUNC(0x3A2A3CA5, BIO_get_cb_arg),
        PS_HLE_FUNC(0x203FBEA3, BIO_get_retry_reason),
        PS_HLE_FUNC(0xCC6A0CB5, cellSsl_CC6A0CB5),
        PS_HLE_FUNC(0x404136C1, cellSsl_404136C1),
        PS_HLE_FUNC(0x2AFE36FF, SSLCERT_OID_to_string),
        // The X509 certificate parser and queries (the next part).
        PS_HLE_FUNC(0x60A5D81F, SSLCERT_from_binary),
        PS_HLE_FUNC(0xB02A43BC, SSLCERT_free),
        PS_HLE_FUNC(0xF798F853, SSLCERT_verify),
        PS_HLE_FUNC(0x005D0928, SSLCERT_check_private_key),
        PS_HLE_FUNC(0x06ECEF1C, SSLCERT_get_subject_name),
        PS_HLE_FUNC(0xA7A0045A, SSLCERT_get_issuer_name),
        PS_HLE_FUNC(0x6CCAAE48, SSLCERT_get_pubkey),
        PS_HLE_FUNC(0x4345A7C1, SSLCERT_get_notBefore),
        PS_HLE_FUNC(0x365B24CE, SSLCERT_get_notAfter),
        PS_HLE_FUNC(0xDD6E2008, SSLCERT_get_extension),
        PS_HLE_FUNC(0x60ACD240, SSLCERT_get_basic_constraints_int),
        PS_HLE_FUNC(0x96022EEF, SSLCERT_NAME_cmp),
        PS_HLE_FUNC(0xF0B47A17, SSLCERT_NAME_oneline),
        PS_HLE_FUNC(0x89902D47, SSLCERT_NAME_get_entry),
        PS_HLE_FUNC(0x9ADAE986, SSLCERT_NAME_get_entry_count),
        PS_HLE_FUNC(0x1CE88676, SSLCERT_NAME_ENTRY_get_info),
        // R_time, EVP, BER, the PEM readers (the ASN.1 parser).
        PS_HLE_FUNC(0x06BCC1DA, R_time),
        PS_HLE_FUNC(0xB829398E, R_time_new),
        PS_HLE_FUNC(0x4C8B7E01, R_time_free),
        PS_HLE_FUNC(0xC4C600F3, R_time_cmp),
        PS_HLE_FUNC(0x86001386, R_time_import),
        PS_HLE_FUNC(0xFFC1A59A, R_time_export),
        PS_HLE_FUNC(0xCB4EDAEC, BER_read_item),
        PS_HLE_FUNC(0x1BE16005, EVP_PKEY_free),
        PS_HLE_FUNC(0x1BBADA6A, _cellSslPemReadPrivateKey),
        PS_HLE_FUNC(0xAF02E7DE, _cellSslPemReadX509),
        // BIO memory/socket subsystem (the next part).
        PS_HLE_FUNC(0x90F2798C, BIO_new_mem),
        PS_HLE_FUNC(0x49033E54, BIO_new_socket),
        PS_HLE_FUNC(0x02C73EB6, BIO_free),
        PS_HLE_FUNC(0xED5D8DD6, BIO_ctrl),
        PS_HLE_FUNC(0xC5B5B2F3, BIO_ptr_ctrl),
        PS_HLE_FUNC(0x3B159B19, BIO_printf),
        PS_HLE_FUNC(0xFF8AC661, BIO_dump),
        // The TLS state machine (the next part).
        PS_HLE_FUNC(0xB4F4A829, SSL_CTX_new),
        PS_HLE_FUNC(0x92A76580, SSL_CTX_free),
        PS_HLE_FUNC(0x38D5898C, SSL_CTX_ctrl),
        PS_HLE_FUNC(0x857024E6, SSL_CTX_set_options),
        PS_HLE_FUNC(0x903F66EB, SSL_CTX_set_verify_mode),
        PS_HLE_FUNC(0xF7D2C560, SSL_CTX_set_info_cb),
        PS_HLE_FUNC(0x1B6643AC, SSL_CTX_set_app_verify_cb),
        PS_HLE_FUNC(0xFE7AEFB7, SSL_CTX_use_certificate),
        PS_HLE_FUNC(0x07296D93, SSL_CTX_use_PrivateKey),
        PS_HLE_FUNC(0x8421B9C7, SSL_new),
        PS_HLE_FUNC(0x7CC3DFE7, SSL_free),
        PS_HLE_FUNC(0x0DBB3F51, SSL_clear),
        PS_HLE_FUNC(0x186E96BC, SSL_set_bio),
        PS_HLE_FUNC(0x0D38E130, SSL_get_rbio),
        PS_HLE_FUNC(0x0266A34C, SSL_set_connect_state),
        PS_HLE_FUNC(0xE55A4804, SSL_set_ssl_method),
        PS_HLE_FUNC(0x01D5F161, SSL_set_session),
        PS_HLE_FUNC(0x2AA1F958, SSL_SESSION_free),
        PS_HLE_FUNC(0xDE8CEF62, SSL_do_handshake),
        PS_HLE_FUNC(0xE5073959, SSL_read),
        PS_HLE_FUNC(0x19726757, SSL_peek),
        PS_HLE_FUNC(0x9F4BEB25, SSL_write),
        PS_HLE_FUNC(0x9E48E5DD, SSL_shutdown),
        PS_HLE_FUNC(0xE34A25C8, SSL_get_error),
        PS_HLE_FUNC(0x9FC527E3, SSL_get_version),
        PS_HLE_FUNC(0x3D80F655, SSL_version),
        PS_HLE_FUNC(0x46714CE1, SSL_state),
        PS_HLE_FUNC(0x84E8B238, SSL_want),
        PS_HLE_FUNC(0x60315DCA, SSL_state_string_long),
        PS_HLE_FUNC(0xF8467DE2, SSL_get_current_cipher),
        PS_HLE_FUNC(0x8C69D32B, SSLv3_client_method),
        PS_HLE_FUNC(0xD78744B4, TLSv1_client_method),
        // The public certificate queries.
        PS_HLE_FUNC(0x7B689EBC, cellSslCertGetSerialNumber),
        PS_HLE_FUNC(0xF8206492, cellSslCertGetPublicKey),
        PS_HLE_FUNC(0x8E505175, cellSslCertGetRsaPublicKeyModulus),
        PS_HLE_FUNC(0x033C4905, cellSslCertGetRsaPublicKeyExponent),
        PS_HLE_FUNC(0x32C61BDF, cellSslCertGetSubjectName),
        PS_HLE_FUNC(0xAE6EB491, cellSslCertGetIssuerName),
        PS_HLE_FUNC(0x31D9BA8D, cellSslCertGetNotBefore),
        PS_HLE_FUNC(0x218B64DA, cellSslCertGetNotAfter),
        PS_HLE_FUNC(0x5E9253CA, cellSslCertGetMd5Fingerprint),
        PS_HLE_FUNC(0x766D3CA1, cellSslCertGetNameEntryCount),
        PS_HLE_FUNC(0x006C4900, cellSslCertGetNameEntryInfo),
        // cellSsl_* certificate helpers libhttp uses.
        PS_HLE_FUNC(0x62236E88, cellSsl_62236E88),
        PS_HLE_FUNC(0x2E8CC56D, cellSsl_2E8CC56D),
        PS_HLE_FUNC(0xC132E990, cellSsl_C132E990),
        PS_HLE_FUNC(0x205B060C, cellSsl_205B060C),
        PS_HLE_FUNC(0x68D3409E, cellSsl_68D3409E),
        PS_HLE_FUNC(0x86C38810, cellSsl_86C38810),
        PS_HLE_FUNC(0x9447404F, cellSsl_9447404F),
        PS_HLE_FUNC(0x06B1E9BF, cellSsl_06B1E9BF),
        PS_HLE_FUNC(0x597BE64A, cellSsl_597BE64A),
        PS_HLE_FUNC(0xF422A5AB, cellSsl_F422A5AB),
        PS_HLE_FUNC(0xC5F77319, cellSsl_C5F77319),
    });
}

} // namespace pspocket::hle::ssl
