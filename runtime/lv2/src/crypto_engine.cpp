// lv2's crypto engine (D-937), ported from the retail kernel's handlers (lv2_kernel.self of the 4.93 system software,
// read in inventory/firmware/kernel493 and never committed; addresses below are the kernel's). RPCS3 binds create,
// destroy and random_generate to todo stubs (CELL_OK, nothing written) and the rest to null_func.
//
// An engine is a kernel object of type 0x78 any process may create. It hashes (HMAC-SHA1) and ciphers with keys
// chosen by index from the system's key tables, which the kernel takes at boot from a validated blob outside its own
// image: they are not pspocket's to have. pspocket's engines meet what a console meets when that blob does not
// validate: preparing a hasher or a cipher fails (CELL_ENOENT), so running one or reading its hash finds it
// unprepared (CELL_EINVAL). Every other check is the kernel's, in its order: which index and mode a process below the
// system software's level may use (index 1, mode 0 or 1; else CELL_EPERM), the key copied in, the engine looked up
// (CELL_ESRCH), the hash output's size (20 bytes at least, else CELL_EFAULT), the data copied in 1 KB at a time.
// random_generate gives 16 bytes from the host's random source, whatever larger size is asked for.

#include <random>

#include "internal.hpp"
#include "pspocket/core/chaos.hpp"

namespace pspocket::lv2 {

namespace {

struct CryptoEngine : Object {
    static constexpr u32 kIdBase = 0x78000000; // the kernel's object type 0x78
    u32 id_base() const override { return kIdBase; }
};

// The calling process's control flags reach the system software's level (dbg.cpp).
bool system_level(PpuThread& t) { return t.kernel.config().root; }

std::shared_ptr<CryptoEngine> engine(PpuThread& t, u32 id)
{
    return t.kernel.impl().objects.get<CryptoEngine>(id);
}

// The key tables (0x80000000000588AC through 0x8000000000057B04): none here, as when the blob does not validate.
Result no_keys() { return err(CELL_ENOENT); }

// Data copied in 1 KB at a time (the hasher's and cipher's runs): CELL_EFAULT at the first piece not guest memory.
Result copy_in_pieces(PpuThread& t, u32 data, s32 size)
{
    const Guest& g = t.kernel.impl().guest;
    for (u32 at = data; size > 0;) {
        const u32 n = static_cast<u32>(std::min(size, 0x400));
        if (!g.valid(at, n)) return err(CELL_EFAULT);
        size -= static_cast<s32>(n);
        at += n;
        // The engine is unprepared (no keys): the first piece is refused before anything is hashed or ciphered.
        return err(CELL_EINVAL);
    }
    return CELL_OK;
}

} // namespace

// sys_crypto_engine_create(u32* id), 0x8000000000089058: the engine's ID, CELL_EFAULT (and no engine) when it cannot
// be written.
Result sys_crypto_engine_create(PpuThread& t, u32 id)
{
    Kernel::Impl& s = t.kernel.impl();
    auto e = std::make_shared<CryptoEngine>();
    e->refs = 1;
    const u32 n = s.objects.insert(CryptoEngine::kIdBase, e);
    if (!n) return err(CELL_EAGAIN);
    if (!s.guest.put32(id, n)) {
        s.objects.remove(CryptoEngine::kIdBase, n);
        return err(CELL_EFAULT);
    }
    return CELL_OK;
}

// sys_crypto_engine_destroy(id), 0x8000000000089A10.
Result sys_crypto_engine_destroy(PpuThread& t, u32 id)
{
    if (!t.kernel.impl().objects.remove(CryptoEngine::kIdBase, id)) return err(CELL_ESRCH);
    return CELL_OK;
}

// sys_crypto_engine_hasher_prepare(id, index), 0x80000000000898CC.
Result sys_crypto_engine_hasher_prepare(PpuThread& t, u32 id, u64 index)
{
    if (!system_level(t) && index != 1) return err(CELL_EPERM);
    if (!engine(t, id)) return err(CELL_ESRCH);
    return no_keys();
}

// sys_crypto_engine_hasher_run(id, data, size), 0x800000000008979C.
Result sys_crypto_engine_hasher_run(PpuThread& t, u32 id, u32 data, s32 size)
{
    if (!engine(t, id)) return err(CELL_ESRCH);
    return copy_in_pieces(t, data, size);
}

// sys_crypto_engine_hasher_get_hash(id, out, size), 0x8000000000089688: the output's size first.
Result sys_crypto_engine_hasher_get_hash(PpuThread& t, u32 id, u32, u64 size)
{
    if (size < 20) return err(CELL_EFAULT);
    if (!engine(t, id)) return err(CELL_ESRCH);
    return err(CELL_EINVAL); // unprepared
}

// sys_crypto_engine_cipher_prepare(id, table, mode, ?, index, key, key_size), 0x8000000000089430: a process below the
// system software's level may use table 1, key index 1 and mode 0 or 1; the key is copied in before the engine is
// looked up.
Result sys_crypto_engine_cipher_prepare(PpuThread& t, u32 id, u64 table, u32 mode, u64 index, u32 key, u64 key_size)
{
    if (!system_level(t) && (table != 1 || index != 1 || mode > 1)) return err(CELL_EPERM);
    if (key_size && !t.kernel.impl().guest.valid(key, static_cast<u32>(std::min<u64>(key_size, 0xFFFFFFFF)))) {
        return err(CELL_EFAULT);
    }
    if (!engine(t, id)) return err(CELL_ESRCH);
    return no_keys();
}

// sys_crypto_engine_cipher_run(id, in, out, size), 0x80000000000892C4.
Result sys_crypto_engine_cipher_run(PpuThread& t, u32 id, u32 in, u32, s32 size)
{
    if (!engine(t, id)) return err(CELL_ESRCH);
    return copy_in_pieces(t, in, size);
}

// sys_crypto_engine_cipher_get_hash(id, out, size), 0x80000000000891B0.
Result sys_crypto_engine_cipher_get_hash(PpuThread& t, u32 id, u32, u64 size)
{
    if (size < 20) return err(CELL_EFAULT);
    if (!engine(t, id)) return err(CELL_ESRCH);
    return err(CELL_EINVAL); // unprepared
}

// sys_crypto_engine_random_generate(buffer, size), 0x8000000000088F78: 16 random bytes; less than 16 asked: CELL_EINVAL.
Result sys_crypto_engine_random_generate(PpuThread& t, u32 buffer, u64 size)
{
    if (size < 16) return err(CELL_EINVAL);
    const Guest& g = t.kernel.impl().guest;
    if (!g.valid(buffer, 16)) return err(CELL_EFAULT);
    for (u32 i = 0; i < 16; i += 4) g.w32(buffer + i, chaos::random32()); // shake mode: its seed's (D-1045)
    return CELL_OK;
}

void register_crypto_engine_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 976, sys_crypto_engine_create);
    PS_LV2_BIND(table, 977, sys_crypto_engine_destroy);
    PS_LV2_BIND(table, 978, sys_crypto_engine_hasher_prepare);
    PS_LV2_BIND(table, 979, sys_crypto_engine_hasher_run);
    PS_LV2_BIND(table, 980, sys_crypto_engine_hasher_get_hash);
    PS_LV2_BIND(table, 981, sys_crypto_engine_cipher_prepare);
    PS_LV2_BIND(table, 982, sys_crypto_engine_cipher_run);
    PS_LV2_BIND(table, 983, sys_crypto_engine_cipher_get_hash);
    PS_LV2_BIND(table, 984, sys_crypto_engine_random_generate);
}

} // namespace pspocket::lv2
