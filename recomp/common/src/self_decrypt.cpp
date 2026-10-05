#include "pspocket/recomp/self_decrypt.hpp"

#include <algorithm>
#include <cstring>

#include "pspocket/recomp/bytes.hpp"
#include "pspocket/recomp/sce.hpp"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#endif
#if __has_include(<zlib.h>)
#define PSPOCKET_SELF_ZLIB 1
#include <zlib.h>
#endif

namespace pspocket::recomp {

namespace {

constexpr usize kMetadataInfoSize = 0x40;    // key, key pad, IV, IV pad
constexpr usize kMetadataHeaderSize = 0x20;
constexpr usize kMetadataSectionSize = 0x30;

struct MetadataSection {
    u64 offset = 0;
    u64 size = 0;
    u32 type = 0;        // 1 section headers, 2 a program header's segment, 3 the SCE version
    u32 program_index = 0;
    u32 encrypted = 0;   // 3: AES-128-CTR with its key and IV
    u32 key_index = 0;
    u32 iv_index = 0;
    u32 compressed = 0;  // 2: zlib
};

#if defined(__APPLE__)
bool aes256_cbc_decrypt(const u8* key, const u8* iv, const u8* in, usize size, u8* out)
{
    usize moved = 0;
    return CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, 32, iv, in, size, out, size, &moved) == kCCSuccess &&
        moved == size;
}

// The 128-bit counter counts big-endian (mbedtls aes_crypt_ctr, as RPCS3 uses it).
bool aes128_ctr(const u8* key, const u8* iv, const u8* in, usize size, u8* out)
{
    CCCryptorRef c = nullptr;
    if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, iv, key, 16, nullptr, 0, 0,
            kCCModeOptionCTR_BE, &c) != kCCSuccess) {
        return false;
    }
    usize moved = 0;
    const bool ok = CCCryptorUpdate(c, in, size, out, size, &moved) == kCCSuccess && moved == size;
    CCCryptorRelease(c);
    return ok;
}
#endif

void put(std::vector<u8>& out, u64 offset, std::span<const u8> bytes)
{
    if (out.size() < offset + bytes.size()) out.resize(static_cast<usize>(offset + bytes.size()));
    std::copy(bytes.begin(), bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace

const SelfKey* find_self_key(std::span<const SelfKey> keys, u32 type, u32 revision, u64 version)
{
    // RPCS3 KeyVault::FindSelfKey: the first entry of the type that fits.
    for (const SelfKey& k : keys) {
        if (k.type != type) continue;
        switch (type) {
        case 1: case 6: return &k;                                                  // LV0, LDR
        case 2: case 3: case 7:                                                     // LV1, LV2, UNK7
            if (version >= k.version_start && version <= k.version_end) return &k;
            break;
        case 4: case 8:                                                             // APP, NPDRM
            if (k.revision == revision) return &k;
            break;
        case 5:                                                                     // ISO
            if (version >= k.version_start && version <= k.version_end && k.revision == revision) return &k;
            break;
        default: break;
        }
    }
    return nullptr;
}

Result<std::vector<u8>> decrypt_self(std::span<const u8> self, std::span<const SelfKey> keys)
{
    auto parsed = parse_self(self);
    if (!parsed) return parsed.error();
    const SelfInfo& info = *parsed;
    if (info.debug && info.plaintext_elf_offset && *info.plaintext_elf_offset < self.size()) {
        // A debug SELF carries its ELF in plain text (RPCS3 CheckDebugSelf).
        const auto plain = self.subspan(static_cast<usize>(*info.plaintext_elf_offset));
        return std::vector<u8>(plain.begin(), plain.end());
    }
    if (info.sce.type != 1 || !info.ext || !info.app || !info.elf) {
        return make_error("not a SELF executable{}", info.warnings.empty() ? "" : ": " + info.warnings.front());
    }
    if (const NpdrmInfo* npd = info.npdrm()) {
        return make_error("a PSN executable ({}, {}): it needs its licence, which disc games do not",
            npd->content_id, npdrm_license_name(npd->license));
    }
    const SelfKey* key = find_self_key(keys, info.app->self_type, info.sce.key_revision, info.app->version);
    if (!key) {
        if (keys.empty()) return make_error("this build carries no SELF keys (scripts/self-keys.py had no RPCS3 source)");
        return make_error("no key for a {} executable of key revision {:#x}", self_type_name(info.app->self_type),
            info.sce.key_revision);
    }
#if !defined(__APPLE__)
    return make_error("decrypting needs CommonCrypto (Apple platforms only)");
#else
    // Metadata info, right after the SCE header at the metadata offset: AES-256-CBC with the key (LoadMetadata).
    const ByteReader r(self);
    const u64 info_offset = u64{info.sce.metadata_offset} + 0x20;
    if (!r.in_bounds(info_offset, kMetadataInfoSize) || info.sce.header_size < info_offset + kMetadataInfoSize ||
        info.sce.header_size > self.size()) {
        return make_error("metadata outside the file");
    }
    std::array<u8, kMetadataInfoSize> meta_info{};
    if (!aes256_cbc_decrypt(key->erk.data(), key->riv.data(), self.data() + info_offset, kMetadataInfoSize,
            meta_info.data())) {
        return make_error("AES-256-CBC failed");
    }
    // A wrong key leaves the pads non-zero (RPCS3 LoadMetadata's check).
    if (meta_info[0x10] != 0 || meta_info[0x30] != 0) {
        return make_error("the {} key for revision {:#x} does not open the metadata (a damaged file?)",
            self_type_name(info.app->self_type), info.sce.key_revision);
    }
    const u8* meta_key = meta_info.data();
    const u8* meta_iv = meta_info.data() + 0x20;

    // The metadata header, section headers and data keys: AES-128-CTR with the key and IV just found.
    const u64 meta_offset = info_offset + kMetadataInfoSize;
    std::vector<u8> meta(static_cast<usize>(info.sce.header_size - meta_offset));
    if (!aes128_ctr(meta_key, meta_iv, self.data() + meta_offset, meta.size(), meta.data())) {
        return make_error("AES-128-CTR failed");
    }
    const ByteReader m(meta);
    if (!m.in_bounds(0, kMetadataHeaderSize)) return make_error("metadata header truncated");
    const u32 section_count = *m.be32(0x0C);
    const u32 key_count = *m.be32(0x10);
    const u64 keys_offset = kMetadataHeaderSize + u64{section_count} * kMetadataSectionSize;
    if (!m.in_bounds(kMetadataHeaderSize, u64{section_count} * kMetadataSectionSize) ||
        !m.in_bounds(keys_offset, u64{key_count} * 16)) {
        return make_error("metadata of {} sections and {} keys runs past its header", section_count, key_count);
    }
    std::vector<MetadataSection> sections(section_count);
    for (u32 i = 0; i < section_count; i++) {
        const u64 o = kMetadataHeaderSize + u64{i} * kMetadataSectionSize;
        MetadataSection& s = sections[i];
        s.offset = *m.be64(o);
        s.size = *m.be64(o + 0x08);
        s.type = *m.be32(o + 0x10);
        s.program_index = *m.be32(o + 0x14);
        s.encrypted = *m.be32(o + 0x20);
        s.key_index = *m.be32(o + 0x24);
        s.iv_index = *m.be32(o + 0x28);
        s.compressed = *m.be32(o + 0x2C);
    }
    const u8* data_keys = meta.data() + keys_offset;

    // The ELF (MakeElf): its header and program headers as the SELF keeps them, each segment at its offset, then the
    // section headers.
    const ElfHeader& eh = *info.elf;
    if (!eh.is64()) return make_error("a 32-bit ELF inside the SELF (only PPU executables are handled)");
    std::vector<u8> elf;
    put(elf, 0, self.subspan(static_cast<usize>(info.ext->elf_offset), eh.header_size()));
    if (!r.in_bounds(info.ext->phdr_offset, u64{eh.phnum} * eh.phdr_size())) return make_error("program headers outside the file");
    put(elf, eh.phoff, self.subspan(static_cast<usize>(info.ext->phdr_offset), usize{eh.phnum} * eh.phdr_size()));
    std::vector<u8> plain;
    for (const MetadataSection& s : sections) {
        if (s.type != 2) continue; // the section headers and the version come from the plain copies
        if (s.program_index >= info.phdrs.size()) return make_error("section for program header {} of {}", s.program_index, info.phdrs.size());
        if (!r.in_bounds(s.offset, s.size)) return make_error("segment {} outside the file", s.program_index);
        plain.assign(self.begin() + static_cast<std::ptrdiff_t>(s.offset),
            self.begin() + static_cast<std::ptrdiff_t>(s.offset + s.size));
        if (s.encrypted == 3) {
            if (s.key_index >= key_count || s.iv_index >= key_count) return make_error("segment {} names key {} of {}", s.program_index, s.key_index, key_count);
            if (!aes128_ctr(data_keys + 16 * s.key_index, data_keys + 16 * s.iv_index, plain.data(), plain.size(),
                    plain.data())) {
                return make_error("AES-128-CTR failed");
            }
        }
        const ProgramHeader& ph = info.phdrs[s.program_index];
        if (s.compressed == 2) {
#if defined(PSPOCKET_SELF_ZLIB)
            std::vector<u8> inflated(static_cast<usize>(ph.filesz));
            uLongf size = static_cast<uLongf>(inflated.size());
            if (uncompress(inflated.data(), &size, plain.data(), static_cast<uLong>(plain.size())) != Z_OK || size != ph.filesz) {
                return make_error("segment {} does not inflate to its {:#x} bytes", s.program_index, ph.filesz);
            }
            put(elf, ph.offset, inflated);
#else
            return make_error("segment {} is compressed and this build has no zlib", s.program_index);
#endif
        } else {
            put(elf, ph.offset, plain);
        }
    }
    if (info.ext->shdr_offset != 0 && eh.shnum != 0) {
        const u64 size = u64{eh.shnum} * eh.shentsize;
        if (!r.in_bounds(info.ext->shdr_offset, size)) return make_error("section headers outside the file");
        put(elf, eh.shoff, self.subspan(static_cast<usize>(info.ext->shdr_offset), static_cast<usize>(size)));
    }
    return elf;
#endif
}

} // namespace pspocket::recomp
