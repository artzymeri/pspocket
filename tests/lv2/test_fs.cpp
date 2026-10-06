// sys_fs over host directories in a temporary folder: /dev_hdd0 (writable), /dev_hdd1, /dev_bdvd (read-only).

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;
namespace fs = std::filesystem;

namespace {

std::atomic<u32> g_dir_counter{0};

struct FsFixture {
    fs::path root;
    Fixture f;
    Caller c{*f.kernel, "fs"};

    FsFixture()
        : root(fs::temp_directory_path() /
              ("pspocket-lv2-fs-" + std::to_string(getpid()) + "-" + std::to_string(g_dir_counter++)))
    {
        fs::remove_all(root);
        fs::create_directories(root / "hdd0" / "game" / "TEST00000");
        fs::create_directories(root / "hdd1");
        fs::create_directories(root / "bdvd" / "PS3_GAME");
        std::ofstream(root / "bdvd" / "PS3_GAME" / "data.bin") << "disc data";
        REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (root / "hdd0").string(), false));
        REQUIRE(f.kernel->vfs_mount("/dev_hdd1", (root / "hdd1").string(), false));
        REQUIRE(f.kernel->vfs_mount("/dev_bdvd", (root / "bdvd").string(), true));
    }
    ~FsFixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    s32 open(std::string_view path, s32 flags, u32& fd)
    {
        const u32 out = f.alloc(4);
        const s32 r = c.call(kFsOpen, f.cstr(path), flags, out, 0666, 0, 0);
        fd = f.r32(out);
        return r;
    }
    u32 open_ok(std::string_view path, s32 flags)
    {
        u32 fd = 0;
        REQUIRE(open(path, flags, fd) == CELL_OK);
        return fd;
    }
    s32 write(u32 fd, std::string_view data, u64& written)
    {
        const u32 buf = f.cstr(data);
        const u32 n = f.alloc(8);
        const s32 r = c.call(kFsWrite, fd, buf, data.size(), n);
        written = f.r64(n);
        return r;
    }
    s32 read(u32 fd, u64 size, std::string& out)
    {
        const u32 buf = f.alloc(static_cast<u32>(size + 1));
        const u32 n = f.alloc(8);
        const s32 r = c.call(kFsRead, fd, buf, size, n);
        out.assign(reinterpret_cast<const char*>(f.base() + buf), static_cast<usize>(f.r64(n)));
        return r;
    }
    s32 stat(std::string_view path, u32 sb) { return c.call(kFsStat, f.cstr(path), sb); }
    std::string host_file(const fs::path& p)
    {
        std::ifstream in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

struct Stat {
    s32 mode, uid, gid;
    u64 size, blksize;
};

Stat read_stat(Fixture& f, u32 sb)
{
    return {static_cast<s32>(f.r32(sb)), static_cast<s32>(f.r32(sb + 4)), static_cast<s32>(f.r32(sb + 8)),
        f.r64(sb + 36), f.r64(sb + 44)};
}

// A read-only image in memory: file paths and contents; directories are the files' parents.
class MemImage final : public VfsImage {
public:
    std::map<std::string, std::string> files;

    class MemFile final : public File {
    public:
        explicit MemFile(std::string bytes) : bytes_(std::move(bytes)) {}
        u64 size() const override { return bytes_.size(); }
        bool read(u64 offset, void* dst, usize size) const override
        {
            if (offset > bytes_.size() || size > bytes_.size() - offset) return false;
            std::memcpy(dst, bytes_.data() + offset, size);
            return true;
        }

    private:
        std::string bytes_;
    };

    std::optional<Stat> stat(std::string_view path) const override
    {
        if (const auto it = files.find(std::string(path)); it != files.end()) return Stat{false, it->second.size()};
        if (path.empty()) return Stat{true, 0};
        const std::string prefix = std::string(path) + "/";
        for (const auto& [p, bytes] : files)
            if (p.starts_with(prefix)) return Stat{true, 0};
        return std::nullopt;
    }
    std::optional<std::vector<std::pair<std::string, Stat>>> list(std::string_view path) const override
    {
        const auto st = stat(path);
        if (!st || !st->dir) return std::nullopt;
        const std::string prefix = path.empty() ? "" : std::string(path) + "/";
        std::map<std::string, Stat> children;
        for (const auto& [p, bytes] : files) {
            if (!p.starts_with(prefix)) continue;
            const std::string rest = p.substr(prefix.size());
            const auto slash = rest.find('/');
            if (slash == std::string::npos) children[rest] = Stat{false, bytes.size()};
            else children[rest.substr(0, slash)] = Stat{true, 0};
        }
        return std::vector<std::pair<std::string, Stat>>(children.begin(), children.end());
    }
    std::shared_ptr<const File> open(std::string_view path) const override
    {
        const auto it = files.find(std::string(path));
        return it == files.end() ? nullptr : std::make_shared<MemFile>(it->second);
    }
};

} // namespace

TEST_CASE("lv2 fs: open, write, read, seek, stat and close on /dev_hdd0", "[lv2][fs]")
{
    FsFixture t;
    const u32 fd = t.open_ok("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_CREAT | CELL_FS_O_RDWR);
    CHECK(fd == 3); // lowest file descriptor (RPCS3 lv2_fs_object id_base 3)
    u64 n = 0;
    REQUIRE(t.write(fd, "hello world", n) == CELL_OK);
    CHECK(n == 11);
    const u32 pos = t.f.alloc(8);
    REQUIRE(t.c.call(kFsLseek, fd, 0, CELL_FS_SEEK_SET, pos) == CELL_OK);
    CHECK(t.f.r64(pos) == 0);
    std::string s;
    REQUIRE(t.read(fd, 5, s) == CELL_OK);
    CHECK(s == "hello");
    REQUIRE(t.c.call(kFsLseek, fd, -5, CELL_FS_SEEK_END, pos) == CELL_OK);
    CHECK(t.f.r64(pos) == 6);
    REQUIRE(t.read(fd, 100, s) == CELL_OK);
    CHECK(s == "world");
    REQUIRE(t.read(fd, 100, s) == CELL_OK);
    CHECK(s.empty()); // end of file
    CHECK(t.c.call(kFsLseek, fd, 0, 3, pos) == rc(CELL_EINVAL));
    CHECK(t.c.call(kFsLseek, fd, -100, CELL_FS_SEEK_SET, pos) == rc(CELL_EINVAL));

    const u32 sb = t.f.alloc(kCellFsStatSize);
    REQUIRE(t.c.call(kFsFstat, fd, sb) == CELL_OK);
    Stat st = read_stat(t.f, sb);
    CHECK(st.mode == (CELL_FS_S_IFREG | 0666));
    CHECK(st.uid == 0); // dev_hdd0 reports uid/gid 0
    CHECK(st.size == 11);
    CHECK(st.blksize == 4096);
    CHECK(t.c.call(kFsFsync, fd) == CELL_OK);
    REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);
    CHECK(t.c.call(kFsClose, fd) == rc(CELL_EBADF));
    CHECK(t.host_file(t.root / "hdd0" / "game" / "TEST00000" / "test.txt") == "hello world");

    REQUIRE(t.stat("/dev_hdd0/game/TEST00000/test.txt", sb) == CELL_OK);
    CHECK(read_stat(t.f, sb).size == 11);
    REQUIRE(t.stat("/dev_hdd0", sb) == CELL_OK);
    st = read_stat(t.f, sb);
    CHECK(st.mode == (CELL_FS_S_IFDIR | 0777));
    CHECK(st.size == 4096); // directories report the block size
    REQUIRE(t.stat("/", sb) == CELL_OK);
    CHECK(read_stat(t.f, sb).mode == (CELL_FS_S_IFDIR | 0711));

    // Flags.
    u32 bad = 0;
    CHECK(t.open("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_CREAT | CELL_FS_O_EXCL | CELL_FS_O_WRONLY, bad) == rc(CELL_EEXIST));
    CHECK(t.open("/dev_hdd0/game/TEST00000/missing.txt", CELL_FS_O_RDONLY, bad) == rc(CELL_ENOENT));
    CHECK(t.open("/dev_hdd0", CELL_FS_O_RDONLY, bad) == rc(CELL_EISDIR));
    CHECK(t.open("dev_hdd0/test.txt", CELL_FS_O_RDONLY, bad) == rc(CELL_ENOENT)); // relative
    CHECK(t.open("/dev_usb000/x", CELL_FS_O_RDONLY, bad) == rc(CELL_ENOTMOUNTED));
    CHECK(t.open("/dev_hdd0/game/TEST00000/" + std::string(1020, 'a'), CELL_FS_O_RDONLY, bad) == rc(CELL_ENAMETOOLONG));
    CHECK(t.open("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_ACCMODE, bad) == rc(CELL_EINVAL));
    CHECK(t.c.call(kFsOpen, 0, CELL_FS_O_RDONLY, t.f.alloc(4), 0, 0, 0) == rc(CELL_EFAULT));

    u32 wfd = t.open_ok("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_WRONLY | CELL_FS_O_APPEND);
    REQUIRE(t.write(wfd, "!", n) == CELL_OK);
    CHECK(t.read(wfd, 1, s) == rc(CELL_EBADF)); // write-only
    CHECK(t.read(wfd, 0, s) == CELL_OK);        // zero bytes skips the access check
    REQUIRE(t.c.call(kFsClose, wfd) == CELL_OK);
    CHECK(t.host_file(t.root / "hdd0" / "game" / "TEST00000" / "test.txt") == "hello world!");
    const u32 rfd = t.open_ok("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_RDONLY);
    CHECK(t.write(rfd, "x", n) == rc(CELL_EBADF)); // read-only
    CHECK(n == 0);
    CHECK(t.c.call(kFsFsync, rfd) == rc(CELL_EBADF));
    CHECK(t.c.call(kFsFtruncate, rfd, 0) == rc(CELL_EBADF));
    REQUIRE(t.c.call(kFsClose, rfd) == CELL_OK);
    wfd = t.open_ok("/dev_hdd0/game/TEST00000/test.txt", CELL_FS_O_WRONLY | CELL_FS_O_TRUNC);
    REQUIRE(t.c.call(kFsClose, wfd) == CELL_OK);
    CHECK(t.host_file(t.root / "hdd0" / "game" / "TEST00000" / "test.txt").empty());
}

TEST_CASE("lv2 fs: file descriptors are the lowest free numbers", "[lv2][fs]")
{
    FsFixture t;
    std::ofstream(t.root / "hdd0" / "game" / "TEST00000" / "a") << "a";
    const u32 a = t.open_ok("/dev_hdd0/game/TEST00000/a", CELL_FS_O_RDONLY);
    const u32 b = t.open_ok("/dev_hdd0/game/TEST00000/a", CELL_FS_O_RDONLY);
    const u32 c = t.open_ok("/dev_hdd0/game/TEST00000/a", CELL_FS_O_RDONLY);
    CHECK(a == 3);
    CHECK(b == 4);
    CHECK(c == 5);
    REQUIRE(t.c.call(kFsClose, b) == CELL_OK);
    CHECK(t.open_ok("/dev_hdd0/game/TEST00000/a", CELL_FS_O_RDONLY) == 4);
    CHECK(t.c.call(kFsClose, 77) == rc(CELL_EBADF));
}

TEST_CASE("lv2 fs: /dev_bdvd is read-only", "[lv2][fs]")
{
    FsFixture t;
    const u32 fd = t.open_ok("/dev_bdvd/PS3_GAME/data.bin", CELL_FS_O_RDONLY);
    std::string s;
    REQUIRE(t.read(fd, 100, s) == CELL_OK);
    CHECK(s == "disc data");
    u64 n = 0;
    CHECK(t.write(fd, "x", n) == rc(CELL_EBADF));
    const u32 sb = t.f.alloc(kCellFsStatSize);
    REQUIRE(t.c.call(kFsFstat, fd, sb) == CELL_OK);
    const Stat st = read_stat(t.f, sb);
    CHECK(st.mode == (CELL_FS_S_IFREG | 0444)); // write bits removed
    CHECK(st.uid == -1);
    CHECK(st.blksize == 2048);
    REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);

    u32 bad = 0;
    CHECK(t.open("/dev_bdvd/PS3_GAME/data.bin", CELL_FS_O_RDWR, bad) == rc(CELL_EPERM));
    CHECK(t.open("/dev_bdvd/new.bin", CELL_FS_O_CREAT | CELL_FS_O_WRONLY, bad) == rc(CELL_EPERM));
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_bdvd/dir"), 0777) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsUnlink, t.f.cstr("/dev_bdvd/PS3_GAME/data.bin")) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsRename, t.f.cstr("/dev_bdvd/PS3_GAME/data.bin"), t.f.cstr("/dev_bdvd/x")) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsTruncate, t.f.cstr("/dev_bdvd/PS3_GAME/data.bin"), 0) == rc(CELL_EROFS));
    CHECK(t.host_file(t.root / "bdvd" / "PS3_GAME" / "data.bin") == "disc data");
}

TEST_CASE("lv2 fs: directories", "[lv2][fs]")
{
    FsFixture t;
    REQUIRE(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir"), 0777) == CELL_OK);
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir"), 0777) == rc(CELL_EEXIST));
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/game/TEST00000/none/sub"), 0777) == rc(CELL_ENOENT));
    // Games may not create entries directly in /dev_hdd0 or /dev_hdd0/game (RPCS3 has_fs_write_rights).
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/game/NEWTITLE"), 0777) == rc(CELL_EACCES));
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/newdir"), 0777) == rc(CELL_EACCES));
    u32 denied = 0;
    CHECK(t.open("/dev_hdd0/new.txt", CELL_FS_O_CREAT | CELL_FS_O_WRONLY, denied) == rc(CELL_EACCES));
    REQUIRE(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd1/cache"), 0777) == CELL_OK); // dev_hdd1 has no such rule
    for (const char* name : {"b", "a", "c"}) {
        const u32 fd = t.open_ok(std::string("/dev_hdd0/game/TEST00000/dir/") + name, CELL_FS_O_CREAT | CELL_FS_O_WRONLY);
        REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);
    }
    REQUIRE(t.c.call(kFsMkdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir/sub"), 0777) == CELL_OK);

    const u32 out = t.f.alloc(4);
    REQUIRE(t.c.call(kFsOpendir, t.f.cstr("/dev_hdd0/game/TEST00000/dir"), out) == CELL_OK);
    const u32 dfd = t.f.r32(out);
    const u32 dirent = t.f.alloc(kCellFsDirentSize);
    const u32 nread = t.f.alloc(8);
    std::vector<std::pair<std::string, u8>> listed;
    for (;;) {
        REQUIRE(t.c.call(kFsReaddir, dfd, dirent, nread) == CELL_OK);
        if (t.f.r64(nread) == 0) break;
        CHECK(t.f.r64(nread) == kCellFsDirentSize);
        const u8 namlen = t.f.base()[dirent + 1];
        listed.emplace_back(std::string(reinterpret_cast<const char*>(t.f.base() + dirent + 2), namlen), t.f.base()[dirent]);
    }
    const std::vector<std::pair<std::string, u8>> expected = {{".", CELL_FS_TYPE_DIRECTORY}, {"..", CELL_FS_TYPE_DIRECTORY},
        {"a", CELL_FS_TYPE_REGULAR}, {"b", CELL_FS_TYPE_REGULAR}, {"c", CELL_FS_TYPE_REGULAR}, {"sub", CELL_FS_TYPE_DIRECTORY}};
    CHECK(listed == expected);
    // Past the end: nread 0 and the first three bytes cleared.
    CHECK(t.f.base()[dirent] == 0);
    CHECK(t.f.base()[dirent + 1] == 0);
    CHECK(t.f.base()[dirent + 2] == 0);
    REQUIRE(t.c.call(kFsClosedir, dfd) == CELL_OK);
    CHECK(t.c.call(kFsReaddir, dfd, dirent, nread) == rc(CELL_EBADF));
    CHECK(t.c.call(kFsClosedir, dfd) == rc(CELL_EBADF));

    CHECK(t.c.call(kFsOpendir, t.f.cstr("/dev_hdd0/game/TEST00000/dir/a"), out) == rc(CELL_ENOTDIR));
    CHECK(t.c.call(kFsOpendir, t.f.cstr("/dev_hdd0/game/TEST00000/nope"), out) == rc(CELL_ENOENT));
    CHECK(t.c.call(kFsOpendir, t.f.cstr("/dev_flash"), out) == rc(CELL_ENOTMOUNTED));

    // The root lists the mount points.
    REQUIRE(t.c.call(kFsOpendir, t.f.cstr("/"), out) == CELL_OK);
    const u32 rfd = t.f.r32(out);
    std::vector<std::string> roots;
    for (;;) {
        REQUIRE(t.c.call(kFsReaddir, rfd, dirent, nread) == CELL_OK);
        if (t.f.r64(nread) == 0) break;
        roots.emplace_back(reinterpret_cast<const char*>(t.f.base() + dirent + 2), t.f.base()[dirent + 1]);
    }
    CHECK(roots == std::vector<std::string>{".", "..", "dev_bdvd", "dev_hdd0", "dev_hdd1"});
    REQUIRE(t.c.call(kFsClosedir, rfd) == CELL_OK);

    CHECK(t.c.call(kFsRmdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir")) == rc(CELL_ENOTEMPTY));
    CHECK(t.c.call(kFsUnlink, t.f.cstr("/dev_hdd0/game/TEST00000/dir/sub")) == rc(CELL_EISDIR));
    CHECK(t.c.call(kFsRmdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir/a")) == rc(CELL_ENOTDIR));
    for (const char* name : {"a", "b", "c"}) REQUIRE(t.c.call(kFsUnlink, t.f.cstr(std::string("/dev_hdd0/game/TEST00000/dir/") + name)) == CELL_OK);
    CHECK(t.c.call(kFsUnlink, t.f.cstr("/dev_hdd0/game/TEST00000/dir/a")) == rc(CELL_ENOENT));
    REQUIRE(t.c.call(kFsRmdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir/sub")) == CELL_OK);
    REQUIRE(t.c.call(kFsRmdir, t.f.cstr("/dev_hdd0/game/TEST00000/dir")) == CELL_OK);
    CHECK_FALSE(fs::exists(t.root / "hdd0" / "game" / "TEST00000" / "dir"));
}

TEST_CASE("lv2 fs: rename, truncate and path normalization", "[lv2][fs]")
{
    FsFixture t;
    const fs::path dir = t.root / "hdd0" / "game" / "TEST00000";
    std::ofstream(dir / "one") << "12345";
    std::ofstream(dir / "two") << "x";
    REQUIRE(t.c.call(kFsRename, t.f.cstr("/dev_hdd0/game/TEST00000/one"), t.f.cstr("/dev_hdd0/game/TEST00000/uno")) == CELL_OK);
    CHECK(fs::exists(dir / "uno"));
    CHECK(t.c.call(kFsRename, t.f.cstr("/dev_hdd0/game/TEST00000/uno"), t.f.cstr("/dev_hdd0/game/TEST00000/two")) == rc(CELL_EEXIST));
    CHECK(t.c.call(kFsRename, t.f.cstr("/dev_hdd0/game/TEST00000/uno"), t.f.cstr("/dev_hdd1/uno")) == rc(CELL_EXDEV));
    CHECK(t.c.call(kFsRename, t.f.cstr("/dev_hdd0/game/TEST00000/none"), t.f.cstr("/dev_hdd0/game/TEST00000/x")) == rc(CELL_ENOENT));
    CHECK(t.c.call(kFsRename, t.f.cstr("/"), t.f.cstr("/dev_hdd0/game/TEST00000/x")) == rc(CELL_EPERM));

    REQUIRE(t.c.call(kFsTruncate, t.f.cstr("/dev_hdd0/game/TEST00000/uno"), 3) == CELL_OK);
    CHECK(fs::file_size(dir / "uno") == 3);
    CHECK(t.c.call(kFsTruncate, t.f.cstr("/dev_hdd0/game/TEST00000/none"), 3) == rc(CELL_ENOENT));
    const u32 fd = t.open_ok("/dev_hdd0/game/TEST00000/uno", CELL_FS_O_RDWR);
    REQUIRE(t.c.call(kFsFtruncate, fd, 10) == CELL_OK);
    CHECK(fs::file_size(dir / "uno") == 10);
    REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);

    // "." and ".." are resolved before the mount lookup; the root's parent is the root.
    const u32 sb = t.f.alloc(kCellFsStatSize);
    CHECK(t.stat("/dev_hdd0/./game/TEST00000/../TEST00000/uno", sb) == CELL_OK);
    CHECK(read_stat(t.f, sb).size == 10);
    CHECK(t.stat("/../../dev_hdd0//game/TEST00000/uno", sb) == CELL_OK);
    CHECK(t.stat("/dev_hdd0/../../../etc/passwd", sb) == rc(CELL_ENOTMOUNTED)); // can't climb out of a mount
    CHECK(t.stat("/dev_hdd0/game/TEST00000/uno/.", sb) == rc(CELL_ENOTDIR));
    CHECK(t.stat("/dev_hdd0/game/.", sb) == CELL_OK);
    CHECK(t.stat("relative", sb) == rc(CELL_ENOENT));
}

TEST_CASE("lv2 fs: a disc image mounted read-only as /dev_bdvd and /app_home", "[lv2][fs]")
{
    FsFixture t;
    auto image = std::make_shared<MemImage>();
    std::string big(5000, '\0');
    for (usize i = 0; i < big.size(); i++) big[i] = static_cast<char>('a' + i % 26);
    image->files["PS3_GAME/PARAM.SFO"] = "sfo";
    image->files["PS3_GAME/USRDIR/EBOOT.BIN"] = "SCE\0eboot";
    image->files["PS3_GAME/USRDIR/data/big.bin"] = big;
    REQUIRE(t.f.kernel->vfs_mount_image("/dev_bdvd", image));
    REQUIRE(t.f.kernel->vfs_mount_image("/app_home", image, "PS3_GAME/USRDIR"));
    CHECK_FALSE(t.f.kernel->vfs_mount_image("/dev_usb000", image, "PS3_GAME/PARAM.SFO")); // not a directory

    // Reads, seeks and the file position, through both mount points.
    const u32 fd = t.open_ok("/dev_bdvd/PS3_GAME/USRDIR/data/big.bin", CELL_FS_O_RDONLY);
    std::string s;
    REQUIRE(t.read(fd, 10, s) == CELL_OK);
    CHECK(s == big.substr(0, 10));
    const u32 pos = t.f.alloc(8);
    REQUIRE(t.c.call(kFsLseek, fd, 4000, CELL_FS_SEEK_SET, pos) == CELL_OK);
    REQUIRE(t.read(fd, 2000, s) == CELL_OK);
    CHECK(s == big.substr(4000)); // 1000 bytes: the rest of the file
    REQUIRE(t.read(fd, 100, s) == CELL_OK);
    CHECK(s.empty()); // at the end
    REQUIRE(t.c.call(kFsLseek, fd, -10, CELL_FS_SEEK_END, pos) == CELL_OK);
    CHECK(t.f.r64(pos) == 4990);
    CHECK(t.c.call(kFsLseek, fd, -5000, CELL_FS_SEEK_CUR, pos) == rc(CELL_EINVAL));
    REQUIRE(t.read(fd, 100, s) == CELL_OK);
    CHECK(s == big.substr(4990));
    const u32 sb = t.f.alloc(kCellFsStatSize);
    REQUIRE(t.c.call(kFsFstat, fd, sb) == CELL_OK);
    Stat st = read_stat(t.f, sb);
    CHECK(st.mode == (CELL_FS_S_IFREG | 0444));
    CHECK(st.size == 5000);
    CHECK(st.blksize == 2048);
    u64 n = 0;
    CHECK(t.write(fd, "x", n) == rc(CELL_EBADF));
    REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);
    CHECK(t.c.call(kFsClose, fd) == rc(CELL_EBADF));
    const u32 app = t.open_ok("/app_home/data/big.bin", CELL_FS_O_RDONLY);
    REQUIRE(t.read(app, 3, s) == CELL_OK);
    CHECK(s == "abc");
    REQUIRE(t.c.call(kFsClose, app) == CELL_OK);

    // Read-only, as /dev_bdvd always is.
    u32 bad = 0;
    CHECK(t.open("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", CELL_FS_O_RDWR, bad) == rc(CELL_EPERM));
    CHECK(t.open("/dev_bdvd/PS3_GAME/new.bin", CELL_FS_O_CREAT | CELL_FS_O_WRONLY, bad) == rc(CELL_EPERM));
    CHECK(t.open("/dev_bdvd/PS3_GAME/none.bin", CELL_FS_O_RDONLY, bad) == rc(CELL_ENOENT));
    CHECK(t.open("/dev_bdvd/PS3_GAME/USRDIR", CELL_FS_O_RDONLY, bad) == rc(CELL_EISDIR));
    CHECK(t.open("/dev_bdvd/PS3_GAME/PARAM.SFO", CELL_FS_O_RDONLY | CELL_FS_O_MSELF, bad) == rc(CELL_ENOTMSELF));
    CHECK(t.c.call(kFsMkdir, t.f.cstr("/dev_bdvd/dir"), 0777) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsUnlink, t.f.cstr("/dev_bdvd/PS3_GAME/PARAM.SFO")) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsUnlink, t.f.cstr("/dev_bdvd/PS3_GAME/USRDIR")) == rc(CELL_EISDIR));
    CHECK(t.c.call(kFsRename, t.f.cstr("/dev_bdvd/PS3_GAME/PARAM.SFO"), t.f.cstr("/dev_bdvd/x")) == rc(CELL_EROFS));
    CHECK(t.c.call(kFsTruncate, t.f.cstr("/dev_bdvd/PS3_GAME/PARAM.SFO"), 0) == rc(CELL_EROFS));

    // stat: directories report the device's block size as their size.
    REQUIRE(t.stat("/dev_bdvd/PS3_GAME", sb) == CELL_OK);
    st = read_stat(t.f, sb);
    CHECK(st.mode == (CELL_FS_S_IFDIR | 0555));
    CHECK(st.size == 2048);
    CHECK(st.uid == -1);
    REQUIRE(t.stat("/dev_bdvd/PS3_GAME/../PS3_GAME/PARAM.SFO", sb) == CELL_OK);
    CHECK(read_stat(t.f, sb).size == 3);
    CHECK(t.stat("/dev_bdvd/PS3_GAME/PARAM.SFO/.", sb) == rc(CELL_ENOTDIR));
    CHECK(t.stat("/dev_bdvd/nope", sb) == rc(CELL_ENOENT));

    // Directory listings: "." and "..", then the entries by name.
    const u32 out = t.f.alloc(4);
    REQUIRE(t.c.call(kFsOpendir, t.f.cstr("/dev_bdvd/PS3_GAME/USRDIR"), out) == CELL_OK);
    const u32 dfd = t.f.r32(out);
    const u32 dirent = t.f.alloc(kCellFsDirentSize);
    const u32 nread = t.f.alloc(8);
    std::vector<std::pair<std::string, u8>> listed;
    for (;;) {
        REQUIRE(t.c.call(kFsReaddir, dfd, dirent, nread) == CELL_OK);
        if (t.f.r64(nread) == 0) break;
        listed.emplace_back(std::string(reinterpret_cast<const char*>(t.f.base() + dirent + 2), t.f.base()[dirent + 1]),
            t.f.base()[dirent]);
    }
    const std::vector<std::pair<std::string, u8>> expected = {{".", CELL_FS_TYPE_DIRECTORY},
        {"..", CELL_FS_TYPE_DIRECTORY}, {"EBOOT.BIN", CELL_FS_TYPE_REGULAR}, {"data", CELL_FS_TYPE_DIRECTORY}};
    CHECK(listed == expected);
    REQUIRE(t.c.call(kFsClosedir, dfd) == CELL_OK);
    CHECK(t.c.call(kFsOpendir, t.f.cstr("/dev_bdvd/PS3_GAME/PARAM.SFO"), out) == rc(CELL_ENOTDIR));
    CHECK(t.c.call(kFsOpendir, t.f.cstr("/dev_bdvd/missing"), out) == rc(CELL_ENOENT));
}

// D-165: sys_fs_open with the SDATA (0x18000000010) or EDATA (2) type argument on a file starting "NPD\0" reads the
// file through the host's NPD opener. Here a stand-in opener whose "plain" bytes are the encrypted ones after the
// 4-byte magic, each XORed with 0x5A; the real one (recomp::NpdFile) is tested in tests/common and tests/hle.
TEST_CASE("lv2 fs: encrypted SDATA files open through the NPD opener, on an image and on a host directory",
    "[lv2][fs][npd]")
{
    FsFixture t;
    class XorFile final : public VfsImage::File {
    public:
        explicit XorFile(std::shared_ptr<const VfsImage::File> enc) : enc_(std::move(enc)) {}
        u64 size() const override { return enc_->size() - 4; }
        bool read(u64 offset, void* dst, usize size) const override
        {
            if (!enc_->read(offset + 4, dst, size)) return false;
            for (usize i = 0; i < size; i++) static_cast<u8*>(dst)[i] ^= 0x5A;
            return true;
        }

    private:
        std::shared_ptr<const VfsImage::File> enc_;
    };
    std::string plain = "the game's own shader programs";
    std::string encrypted = std::string("NPD\0", 4);
    for (char c : plain) encrypted += static_cast<char>(c ^ 0x5A);
    auto image = std::make_shared<MemImage>();
    image->files["PS3_GAME/USRDIR/programs.sdat"] = encrypted;
    image->files["PS3_GAME/USRDIR/plain.sdat"] = "not encrypted";
    REQUIRE(t.f.kernel->vfs_mount_image("/dev_bdvd", image));
    std::ofstream(t.root / "hdd0" / "game" / "TEST00000" / "programs.sdat", std::ios::binary) << encrypted;

    const u32 type = t.f.alloc(8);
    t.f.w32(type, 0x180);
    t.f.w32(type + 4, 0x10);
    const u32 out = t.f.alloc(4);
    auto open_sdata = [&](std::string_view path, s32 flags) {
        return t.c.call(kFsOpen, t.f.cstr(path), flags, out, 0, type, 8);
    };

    // No opener: refused, as before (RPCS3's answer when the header does not open).
    CHECK(open_sdata("/dev_bdvd/PS3_GAME/USRDIR/programs.sdat", CELL_FS_O_RDONLY) == rc(CELL_EFSSPECIFIC));

    bool asked_edata = true;
    t.f.kernel->set_npd_opener([&](std::shared_ptr<const VfsImage::File> enc, bool edata,
                                   std::string& error) -> std::shared_ptr<const VfsImage::File> {
        asked_edata = edata;
        u8 magic[4]{};
        if (!enc->read(0, magic, 4) || std::memcmp(magic, "NPD\0", 4) != 0) {
            error = "not NPD";
            return nullptr;
        }
        return std::make_shared<XorFile>(std::move(enc));
    });
    for (const char* path : {"/dev_bdvd/PS3_GAME/USRDIR/programs.sdat", "/dev_hdd0/game/TEST00000/programs.sdat"}) {
        INFO(path);
        REQUIRE(open_sdata(path, CELL_FS_O_RDONLY) == CELL_OK);
        CHECK_FALSE(asked_edata);
        const u32 fd = t.f.r32(out);
        std::string s;
        REQUIRE(t.read(fd, 100, s) == CELL_OK);
        CHECK(s == plain);
        const u32 pos = t.f.alloc(8);
        REQUIRE(t.c.call(kFsLseek, fd, 4, CELL_FS_SEEK_SET, pos) == CELL_OK);
        REQUIRE(t.read(fd, 4, s) == CELL_OK);
        CHECK(s == "game");
        const u32 sb = t.f.alloc(kCellFsStatSize);
        REQUIRE(t.c.call(kFsFstat, fd, sb) == CELL_OK);
        CHECK(read_stat(t.f, sb).size == plain.size()); // the plain size, as RPCS3's EDATADecrypter reports
        u64 n = 0;
        CHECK(t.write(fd, "x", n) == rc(CELL_EBADF));
        REQUIRE(t.c.call(kFsClose, fd) == CELL_OK);
    }
    // Writing is refused; a file without the magic opens as it is; EDATA asks the opener for EDATA.
    CHECK(open_sdata("/dev_hdd0/game/TEST00000/programs.sdat", CELL_FS_O_RDWR) == rc(CELL_EPERM));
    REQUIRE(open_sdata("/dev_bdvd/PS3_GAME/USRDIR/plain.sdat", CELL_FS_O_RDONLY) == CELL_OK);
    std::string s;
    REQUIRE(t.read(t.f.r32(out), 100, s) == CELL_OK);
    CHECK(s == "not encrypted");
    t.f.w32(type, 0);
    t.f.w32(type + 4, 2);
    REQUIRE(open_sdata("/dev_bdvd/PS3_GAME/USRDIR/programs.sdat", CELL_FS_O_RDONLY) == CELL_OK);
    CHECK(asked_edata);
    // Opened without the type argument, the file is its encrypted bytes (as RPCS3).
    REQUIRE(t.c.call(kFsOpen, t.f.cstr("/dev_bdvd/PS3_GAME/USRDIR/programs.sdat"), CELL_FS_O_RDONLY, out, 0, 0, 0) ==
            CELL_OK);
    REQUIRE(t.read(t.f.r32(out), 4, s) == CELL_OK);
    CHECK(s == std::string("NPD\0", 4));
}
