// sys_libc's stdio and POSIX file functions (runtime/hle/src/sys_libc_stdio.cpp, D-514): streams on lv2's file
// system (host directories mounted as /dev_hdd0 and /dev_hdd1) and its TTY, each function called as guest code calls
// it. No game data.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace fsys = std::filesystem;

namespace {

constexpr u32 kPath = kData + 0x1000, kMode = kData + 0x1100, kFmt = kData + 0x1200, kStr = kData + 0x1300,
              kBuf = kData + 0x2000, kOut = kData + 0x6000, kBig = kData + 0x10000;
constexpr u32 kEnoent = 0x80010006u, kEinval = 0x80010002u, kEbadf = 0x8001002Au;

void put(u8* b, u32 at, std::string_view s)
{
    std::memcpy(b + at, s.data(), s.size());
    b[at + s.size()] = 0;
}

std::string cstr(u8* b, u32 at)
{
    std::string s;
    while (b[at + s.size()]) s += static_cast<char>(b[at + s.size()]);
    return s;
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

std::string slurp(const fsys::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fsys::path temp_root(std::string_view what)
{
    const fsys::path root =
        fsys::temp_directory_path() / std::format("pspocket-libc-{}-{}", what, static_cast<long>(getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root / "hdd0" / "d"); // games may not create entries in /dev_hdd0 itself
    fsys::create_directories(root / "hdd1");
    return root;
}

// The TTY's text written while it lives.
struct Tty {
    std::mutex mutex;
    std::string text;
    Tty()
    {
        log::set_sink(
            [](void* user, log::Level, std::string_view channel, std::string_view message) {
                if (channel != "tty") return;
                auto* self = static_cast<Tty*>(user);
                std::lock_guard lock(self->mutex);
                self->text += message;
                self->text += '|';
            },
            this);
    }
    ~Tty() { log::set_sink(nullptr, nullptr); }
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;
    std::string get()
    {
        std::lock_guard lock(mutex);
        return text;
    }
};

struct Libc {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    fsys::path root;

    explicit Libc(std::string_view what) : root(temp_root(what))
    {
        REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
        REQUIRE(f.kernel->vfs_mount("/dev_hdd1", (root / "hdd1").string(), false));
    }
    ~Libc() { fsys::remove_all(root); }

    u32 open(std::string_view path, std::string_view mode)
    {
        put(b, kPath, path);
        put(b, kMode, mode);
        return static_cast<u32>(t.call("fopen", kPath, kMode));
    }
    u32 err() { return ps_read32(b, static_cast<u32>(t.call("_Geterrno"))); }
    u32 std_file(const char* name) { return HleProcess::current()->link_variable("sys_libc", recomp::nid_of(name)); }
};

} // namespace

TEST_CASE("sys_libc: stdio streams on lv2's file system", "[hle][libc][stdio]")
{
    Libc c("streams");
    auto& t = c.t;
    u8* b = c.b;
    // Writing: fputs, fputc, fprintf (its count), fwrite; nothing reaches the file before fclose flushes it.
    const u32 w = c.open("/dev_hdd0/d/a.txt", "w");
    REQUIRE(w != 0);
    put(b, kStr, "first line\n");
    CHECK(i32(t.call("fputs", kStr, w)) >= 0);
    CHECK(t.call("fputc", 'X', w) == 'X');
    put(b, kFmt, "%d-%s-%04x-%.2f\n");
    put(b, kStr, "abc");
    CHECK(i32(t.call("fprintf", w, kFmt, 42, kStr, 0xBEEF, std::bit_cast<u64>(3.14159))) == 17);
    put(b, kStr, "0123456789");
    CHECK(t.call("fwrite", kStr, 2, 5, w) == 5);
    CHECK(slurp(c.root / "hdd0" / "d" / "a.txt").empty());
    CHECK(t.call("fclose", w) == 0);
    CHECK(slurp(c.root / "hdd0" / "d" / "a.txt") == "first line\nX42-abc-beef-3.14\n0123456789");
    // Reading: fgetc, ungetc (more than one byte), fgets, fscanf, fread; feof only after a read hits the end.
    const u32 r = c.open("/dev_hdd0/d/a.txt", "r");
    REQUIRE(r != 0);
    CHECK(t.call("fgetc", r) == 'f');
    CHECK(t.call("ungetc", 'Q', r) == 'Q');
    CHECK(t.call("ungetc", 'P', r) == 'P');
    CHECK(t.call("fgetc", r) == 'P');
    CHECK(t.call("fgetc", r) == 'Q');
    CHECK(t.call("fgets", kBuf, 64, r) == kBuf);
    CHECK(cstr(b, kBuf) == "irst line\n");
    CHECK(t.call("fgets", kBuf, 1, r) == 0); // n of 1: NULL, as Dinkumware's
    put(b, kFmt, "X%d-%3s-%x");
    CHECK(i32(t.call("fscanf", r, kFmt, kOut, kOut + 8, kOut + 16)) == 3);
    CHECK(ps_read32(b, kOut) == 42);
    CHECK(cstr(b, kOut + 8) == "abc");
    CHECK(ps_read32(b, kOut + 16) == 0xBEEF);
    CHECK(i32(t.call("ftell", r)) == 23);
    CHECK(t.call("fseek", r, static_cast<u64>(-10), 2) == 0); // SEEK_END
    CHECK(t.call("fread", kBuf, 1, 100, r) == 10);
    CHECK(std::memcmp(b + kBuf, "0123456789", 10) == 0);
    CHECK(t.call("feof", r) == 1);
    CHECK(i32(t.call("fgetc", r)) == -1);
    t.call("clearerr", r);
    CHECK(t.call("feof", r) == 0);
    // fgetpos and fsetpos, rewind.
    t.call("rewind", r);
    CHECK(t.call("fgetc", r) == 'f');
    CHECK(t.call("fgetpos", r, kOut) == 0);
    CHECK(ps_read64(b, kOut) == 1);
    t.call("fgetc", r);
    t.call("fgetc", r);
    CHECK(t.call("fsetpos", r, kOut) == 0);
    CHECK(t.call("fgetc", r) == 'i');
    // A stream opened for reading cannot be written (EBADF, the error indicator set).
    CHECK(i32(t.call("fputc", 'z', r)) == -1);
    CHECK(t.call("ferror", r) == 1);
    CHECK(t.call("fclose", r) == 0);
    // Appending, and "r+" switching between reading and writing with no seek in between.
    const u32 a = c.open("/dev_hdd0/d/a.txt", "a");
    put(b, kStr, "!");
    t.call("fputs", kStr, a);
    t.call("fclose", a);
    CHECK(slurp(c.root / "hdd0" / "d" / "a.txt").back() == '!');
    const u32 rw = c.open("/dev_hdd0/d/a.txt", "r+");
    CHECK(t.call("fgetc", rw) == 'f');
    CHECK(t.call("fputc", 'F', rw) == 'F'); // overwrites the second byte
    CHECK(t.call("fgetc", rw) == 'r');
    t.call("fclose", rw);
    CHECK(slurp(c.root / "hdd0" / "d" / "a.txt").substr(0, 4) == "fFrs");
    // Errors: a missing file (lv2's ENOENT), a bad mode (EINVAL).
    CHECK(c.open("/dev_hdd0/d/missing.txt", "r") == 0);
    CHECK(c.err() == kEnoent);
    CHECK(c.open("/dev_hdd0/d/a.txt", "q") == 0);
    CHECK(c.err() == kEinval);
    // Large reads and writes go straight between lv2 and the caller's memory.
    for (u32 i = 0; i < 20000; i++) b[kBig + i] = static_cast<u8>(i * 7);
    const u32 big = c.open("/dev_hdd0/d/big.bin", "w+b");
    CHECK(t.call("fwrite", kBig, 4, 5000, big) == 5000);
    t.call("rewind", big);
    std::memset(b + kBig + 0x8000, 0, 20000);
    CHECK(t.call("fread", kBig + 0x8000, 1, 20000, big) == 20000);
    CHECK(std::memcmp(b + kBig, b + kBig + 0x8000, 20000) == 0);
    t.call("fclose", big);
    CHECK(fsys::file_size(c.root / "hdd0" / "d" / "big.bin") == 20000);
    // remove and rename.
    put(b, kPath, "/dev_hdd0/d/big.bin");
    put(b, kStr, "/dev_hdd0/d/moved.bin");
    CHECK(t.call("rename", kPath, kStr) == 0);
    CHECK(fsys::exists(c.root / "hdd0" / "d" / "moved.bin"));
    CHECK(t.call("remove", kStr) == 0);
    CHECK(!fsys::exists(c.root / "hdd0" / "d" / "moved.bin"));
    CHECK(i32(t.call("remove", kStr)) == -1);
    // tmpnam's names are on /dev_hdd1; tmpfile's file goes at fclose.
    const u32 name = static_cast<u32>(t.call("tmpnam", 0));
    REQUIRE(name != 0);
    CHECK(cstr(b, name).starts_with("/dev_hdd1/t"));
    CHECK(cstr(b, name).size() < 16);
    const u32 tmp = static_cast<u32>(t.call("tmpfile"));
    REQUIRE(tmp != 0);
    put(b, kStr, "scratch");
    t.call("fputs", kStr, tmp);
    t.call("rewind", tmp);
    CHECK(t.call("fgets", kBuf, 64, tmp) == kBuf);
    CHECK(cstr(b, kBuf) == "scratch");
    CHECK(std::distance(fsys::directory_iterator(c.root / "hdd1"), fsys::directory_iterator()) == 1);
    t.call("fclose", tmp);
    CHECK(std::distance(fsys::directory_iterator(c.root / "hdd1"), fsys::directory_iterator()) == 0);
    // Wide characters through a byte stream, and fwide's orientation.
    const u32 wf = c.open("/dev_hdd0/d/w.txt", "w+");
    CHECK(i32(t.call("fwide", wf, 0)) == 0);
    CHECK(t.call("fputwc", 0xE9, wf) == 0xE9);
    CHECK(static_cast<u32>(t.call("fputwc", 0x100, wf)) == 0xFFFFFFFFu); // no byte for it: EILSEQ
    CHECK(i32(t.call("fwide", wf, 0)) > 0);
    t.call("rewind", wf);
    CHECK(t.call("fgetwc", wf) == 0xE9);
    t.call("fclose", wf);
}

TEST_CASE("sys_libc: the standard streams on the TTY", "[hle][libc][stdio]")
{
    Libc c("std");
    Tty tty;
    auto& t = c.t;
    u8* b = c.b;
    // stdout is line buffered: printf's text reaches the TTY at the newline.
    put(b, kFmt, "value %d");
    CHECK(i32(t.call("printf", kFmt, 7)) == 7);
    CHECK(tty.get().find("value 7") == std::string::npos);
    put(b, kStr, "!");
    CHECK(i32(t.call("puts", kStr)) >= 0);
    CHECK(tty.get().find("value 7!") != std::string::npos);
    // stderr is unbuffered; perror prints errno's message.
    const u32 err = c.std_file("_Stderr");
    REQUIRE(err != 0);
    CHECK(ps_read32(b, err) == err); // the FILE's first word is its own address
    put(b, kStr, "now");
    t.call("fputs", kStr, err);
    CHECK(tty.get().find("now") != std::string::npos);
    ps_write32(b, static_cast<u32>(t.call("_Geterrno")), kEnoent);
    put(b, kStr, "opening");
    t.call("perror", kStr);
    CHECK(tty.get().find("opening: no such file or directory") != std::string::npos);
    // stdin has no input: getchar is EOF at once and scanf returns EOF.
    CHECK(i32(t.call("getchar")) == -1);
    put(b, kFmt, "%d");
    CHECK(i32(t.call("scanf", kFmt, kOut)) == -1);
    // write(1) and write(2) go to the TTY; read(0) is the end of input.
    put(b, kStr, "raw write");
    CHECK(t.call("write", 1, kStr, 9) == 9);
    CHECK(tty.get().find("raw write") != std::string::npos);
    CHECK(t.call("read", 0, kBuf, 10) == 0);
    CHECK(i32(t.call("read", 1, kBuf, 10)) == -1);
    CHECK(c.err() == kEbadf);
    // fflush(NULL) flushes stdout's partial line.
    put(b, kStr, "partial");
    t.call("fputs", kStr, c.std_file("_Stdout"));
    CHECK(tty.get().find("partial") == std::string::npos);
    CHECK(t.call("fflush", 0) == 0);
    CHECK(tty.get().find("partial") != std::string::npos);
}

TEST_CASE("sys_libc: the printf and scanf families on strings", "[hle][libc][stdio]")
{
    Libc c("strings");
    auto& t = c.t;
    u8* b = c.b;
    put(b, kFmt, "%s=%5d|%-4x|%c|%e");
    put(b, kStr, "key");
    CHECK(i32(t.call("sprintf", kBuf, kFmt, kStr, 123, 0xAB, 'z', std::bit_cast<u64>(1.5))) == 29);
    CHECK(cstr(b, kBuf) == "key=  123|ab  |z|1.500000e+00");
    // snprintf: the whole length, at most n - 1 bytes and a NUL.
    std::memset(b + kBuf, 0x7E, 32);
    CHECK(i32(t.call("snprintf", kBuf, 6, kFmt, kStr, 123, 0xAB, 'z', std::bit_cast<u64>(1.5))) == 29);
    CHECK(cstr(b, kBuf) == "key= ");
    CHECK(b[kBuf + 6] == 0x7E);
    CHECK(i32(t.call("snprintf", 0, 0, kFmt, kStr, 1, 2, 'a', std::bit_cast<u64>(0.0))) == 29);
    // A va_list is the address of its 64-bit slots.
    ps_write64(b, kOut, 77);
    ps_write64(b, kOut + 8, kStr);
    put(b, kFmt, "%d:%s");
    CHECK(i32(t.call("vsprintf", kBuf, kFmt, kOut)) == 6);
    CHECK(cstr(b, kBuf) == "77:key");
    CHECK(i32(t.call("vsnprintf", kBuf, 3, kFmt, kOut)) == 6);
    CHECK(cstr(b, kBuf) == "77");
    // sscanf: conversions, a scanset, %n, and EOF before the first conversion.
    put(b, kStr, "  12 0x1F word rest");
    put(b, kFmt, "%d %i %[a-z]%n");
    CHECK(i32(t.call("sscanf", kStr, kFmt, kOut, kOut + 4, kOut + 8, kOut + 40)) == 3);
    CHECK(ps_read32(b, kOut) == 12);
    CHECK(ps_read32(b, kOut + 4) == 31);
    CHECK(cstr(b, kOut + 8) == "word");
    CHECK(ps_read32(b, kOut + 40) == 14);
    put(b, kStr, "");
    put(b, kFmt, "%d");
    CHECK(i32(t.call("sscanf", kStr, kFmt, kOut)) == -1);
    put(b, kStr, "2.5e1");
    put(b, kFmt, "%lf");
    CHECK(t.call("sscanf", kStr, kFmt, kOut) == 1);
    CHECK(std::bit_cast<double>(ps_read64(b, kOut)) == 25.0);
    // The wide forms: swprintf (negative when a piece does not fit, what fitted written, as the SDK's, D-905) and
    // swscanf.
    const auto wput = [&](u32 at, std::u32string_view s) {
        for (usize i = 0; i < s.size(); i++) ps_write32(b, at + 4 * static_cast<u32>(i), static_cast<u32>(s[i]));
        ps_write32(b, at + 4 * static_cast<u32>(s.size()), 0);
    };
    wput(kFmt, U"%d/%ls");
    wput(kStr, U"wé");
    CHECK(i32(t.call("swprintf", kBuf, 16, kFmt, 5, kStr)) == 4);
    CHECK(ps_read32(b, kBuf) == '5');
    CHECK(ps_read32(b, kBuf + 12) == 0xE9);
    CHECK(ps_read32(b, kBuf + 16) == 0);
    CHECK(i32(t.call("swprintf", kBuf, 3, kFmt, 5, kStr)) < 0);
    CHECK(ps_read32(b, kBuf + 4) == '/');
    CHECK(ps_read32(b, kBuf + 8) == 0);
    wput(kStr, U"41 x");
    wput(kFmt, U"%d %lc");
    CHECK(t.call("swscanf", kStr, kFmt, kOut, kOut + 4) == 2);
    CHECK(ps_read32(b, kOut) == 41);
    CHECK(ps_read32(b, kOut + 4) == 'x');
}

TEST_CASE("sys_libc: the POSIX file functions over lv2", "[hle][libc][stdio]")
{
    Libc c("posix");
    auto& t = c.t;
    u8* b = c.b;
    // open, write, lseek, read, fstat, close; lv2's descriptors start at 3.
    put(b, kPath, "/dev_hdd0/d/p.bin");
    const s32 fd = i32(t.call("open", kPath, 0x40 | 0x2, 0644)); // O_CREAT | O_RDWR
    REQUIRE(fd >= 3);
    put(b, kStr, "posix data");
    CHECK(t.call("write", fd, kStr, 10) == 10);
    CHECK(t.call("lseek", fd, 0, 0) == 0);
    CHECK(t.call("read", fd, kBuf, 64) == 10);
    CHECK(std::memcmp(b + kBuf, "posix data", 10) == 0);
    CHECK(t.call("lseek64", fd, 0, 2) == 10);
    CHECK(t.call("fcntl", fd, 3, 0) == (0x40 | 0x2)); // F_GETFL
    CHECK(t.call("fstat", fd, kOut) == 0);
    CHECK(ps_read64(b, kOut + 36) == 10); // CellFsStat's size
    CHECK(t.call("ftruncate", fd, 4) == 0);
    CHECK(t.call("fsync", fd) == 0);
    CHECK(t.call("close", fd) == 0);
    CHECK(i32(t.call("close", fd)) == -1);
    CHECK(slurp(c.root / "hdd0" / "d" / "p.bin") == "posi");
    CHECK(t.call("stat", kPath, kOut) == 0);
    CHECK(ps_read64(b, kOut + 36) == 4);
    CHECK(t.call("truncate", kPath, 2) == 0);
    CHECK(slurp(c.root / "hdd0" / "d" / "p.bin") == "po");
    // fdopen wraps a descriptor in a stream.
    const s32 fd2 = i32(t.call("open", kPath, 0, 0));
    put(b, kMode, "r");
    const u32 s = static_cast<u32>(t.call("fdopen", fd2, kMode));
    REQUIRE(s != 0);
    CHECK(i32(t.call("fileno", s)) == fd2);
    CHECK(t.call("fgetc", s) == 'p');
    t.call("fclose", s);
    // Directories: mkdir, opendir, readdir (CellFsDirent: type, name length, name), closedir, rmdir.
    put(b, kPath, "/dev_hdd0/d/dir");
    CHECK(t.call("mkdir", kPath, 0755) == 0);
    put(b, kStr, "/dev_hdd0/d/dir/f1");
    t.call("close", t.call("open", kStr, 0x40 | 0x1, 0644));
    const u32 dir = static_cast<u32>(t.call("opendir", kPath));
    REQUIRE(dir != 0);
    std::vector<std::string> names;
    while (const u32 e = static_cast<u32>(t.call("readdir", dir))) names.push_back(cstr(b, e + 2));
    CHECK(std::ranges::count(names, "f1") == 1);
    CHECK(t.call("closedir", dir) == 0);
    CHECK(i32(t.call("rmdir", kPath)) == -1); // not empty
    CHECK(t.call("unlink", kStr) == 0);
    CHECK(t.call("rmdir", kPath) == 0);
    // getpid is lv2's; kill has no signals to send.
    CHECK(i32(t.call("getpid")) > 0);
    CHECK(i32(t.call("kill", t.call("getpid"), 6)) == -1);
}
