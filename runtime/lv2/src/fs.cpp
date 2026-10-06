// sys_fs over a VFS that maps PS3 mount points to host directories (plan section 8, "Filesystem mapping").
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_fs.h/.cpp (flags, error mapping, mount point table, CellFsStat /
// CellFsDirent layouts, readdir quirks), rpcs3/Emu/VFS.cpp (path normalization).
//
// Mount points (configured by the host with Kernel::vfs_mount): /dev_bdvd (read-only), /dev_hdd0, /dev_hdd1,
// /app_home, /host_root (dev only), plus anything else the host mounts. Paths are normalized before mapping ("."
// and ".." resolved, ".." of the root is the root), so a guest path can't name a host file outside its mount.
// A mount can instead show a read-only image (Kernel::vfs_mount_image: the game's disc image read in place); every
// syscall then reads the image, with the same results a read-only host directory gives.
//
// Deviations:
// - Encrypted SDATA/EDATA files (sys_fs_open with the 8-byte type argument and an "NPD\0" header) open through the
//   host's NPD opener (Kernel::set_npd_opener, D-165) as read-only files of their plain bytes; without one, or when
//   it refuses the file, the open fails with CELL_EFSSPECIFIC. Unencrypted ones open as plain files.
// - No split-file (".66600") gathering, no HDD speed emulation, no I/O memory containers.
// - Invalid open flags return CELL_EINVAL with an error log (RPCS3 throws).
// - Lookups are case-sensitive where the host filesystem is (iOS APFS is): extracted assets must keep their case.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kFs = "fs";
constexpr usize kMaxPath = 1024;     // CELL_FS_MAX_FS_PATH_LENGTH
constexpr usize kMaxName = 255;      // CELL_FS_MAX_FS_FILE_NAME_LENGTH
constexpr usize kBounce = 1u << 20;  // host I/O goes through a bounce buffer (write-tracked guest pages fault)

class FsObject : public Object {
public:
    static constexpr u32 kIdBase = 3; // RPCS3 lv2_fs_object: fds 3..254, lowest free first
    u32 id_base() const override { return kIdBase; }
    std::string vpath;
    std::shared_ptr<const Mount> mount;
};

class FsFile final : public FsObject {
public:
    ~FsFile() override
    {
        if (host_fd >= 0) ::close(host_fd);
    }
    // Guarded by lock. A file is open while it has a host descriptor or (on an image mount) an image file.
    int host_fd = -1;
    std::shared_ptr<const VfsImage::File> image_file;
    u64 pos = 0; // image files: the file position (host files use the descriptor's)
    bool is_open() const { return host_fd >= 0 || image_file; }
    s32 flags = 0;
    s32 mode = 0;
};

class FsDir final : public FsObject {
public:
    struct Entry {
        std::string name;
        bool dir = false;
    };
    std::vector<Entry> entries;
    usize pos = 0; // guarded by lock
};

Result map_errno(int e, std::string_view what, std::string_view path)
{
    switch (e) {
    case ENOENT: return err(CELL_ENOENT);
    case ENOTDIR: return err(CELL_ENOTDIR);
    case EISDIR: return err(CELL_EISDIR);
    case EEXIST: return err(CELL_EEXIST);
    case ENOTEMPTY: return err(CELL_ENOTEMPTY);
    case EACCES:
    case EPERM: return err(CELL_EACCES);
    case EROFS: return err(CELL_EROFS);
    case ENOSPC: return err(CELL_ENOSPC);
    case EMFILE:
    case ENFILE: return err(CELL_EMFILE);
    case ENAMETOOLONG: return err(CELL_ENAMETOOLONG);
    case EXDEV: return err(CELL_EXDEV);
    case EINVAL: return err(CELL_EINVAL);
    case EFBIG: return err(CELL_EFBIG);
    case EBADF: return err(CELL_EBADF);
    default:
        PS_LOG_ERROR(kFs, "{} {}: unexpected host error {} ({})", what, path, e, std::strerror(e));
        return err(CELL_EIO);
    }
}

// RPCS3 lv2_fs_mount_point table: block size and uid/gid reporting per device.
void mount_properties(Mount& m)
{
    const std::string_view p = m.point;
    if (p == "/dev_bdvd" || p == "/dev_ps2disc") {
        m.block_size = 2048;
        m.no_uid_gid = true;
    } else if (p == "/dev_hdd0") {
        m.block_size = 4096;
        m.no_uid_gid = false;
    } else if (p == "/dev_hdd1") {
        m.block_size = 32768;
        m.no_uid_gid = true;
    } else if (p == "/app_home" || p == "/host_root") {
        m.block_size = 512;
        m.no_uid_gid = true;
    } else if (p.starts_with("/dev_flash")) {
        m.block_size = 8192;
        m.no_uid_gid = true;
    } else {
        m.block_size = 4096; // /dev_usb* and anything else
        m.no_uid_gid = true;
    }
}

// RPCS3 has_fs_write_rights: games may not create entries directly in /dev_hdd0, /dev_hdd0/game or /dev_flash*.
bool has_write_rights(const Kernel& k, std::string_view vpath)
{
    if (k.config().root) return true;
    const usize slash = vpath.find_last_of('/');
    const std::string_view parent = slash == std::string_view::npos || slash == 0 ? std::string_view("/") : vpath.substr(0, slash);
    if (parent == "/dev_hdd0" || parent == "/dev_hdd0/game") return false;
    if (parent.starts_with("/dev_flash")) return false;
    if (parent.starts_with("/dev_bdvd") || parent == "/") return false;
    return true;
}

struct PathArg {
    Result error = CELL_OK;
    std::string vpath;
};

// RPCS3 translate_to_str: at most 1024 characters, and a path must be absolute.
PathArg read_path(const Guest& g, u32 addr)
{
    PathArg p;
    switch (g.read_cstr(addr, kMaxPath, p.vpath)) {
    case Guest::Str::Ok: break;
    case Guest::Str::Fault: p.error = err(CELL_EFAULT); return p;
    case Guest::Str::TooLong: p.error = err(CELL_ENAMETOOLONG); return p;
    }
    if (!p.vpath.starts_with('/')) p.error = err(CELL_ENOENT);
    return p;
}

void write_stat(const Guest& g, u32 sb, s32 mode, s32 uid, s32 gid, s64 atime, s64 mtime, s64 ctime, u64 size,
    u64 blksize)
{
    // CellFsStat: mode, uid, gid, then 64-bit fields at 4-byte alignment (52 bytes).
    g.w32(sb, static_cast<u32>(mode));
    g.w32(sb + 4, static_cast<u32>(uid));
    g.w32(sb + 8, static_cast<u32>(gid));
    g.w64(sb + 12, static_cast<u64>(atime));
    g.w64(sb + 20, static_cast<u64>(mtime));
    g.w64(sb + 28, static_cast<u64>(ctime));
    g.w64(sb + 36, size);
    g.w64(sb + 44, blksize);
}

void write_host_stat(const Guest& g, u32 sb, const Mount& m, const struct stat& st, bool dir_size_is_block)
{
    const bool dir = S_ISDIR(st.st_mode);
    s32 mode = dir ? (CELL_FS_S_IFDIR | 0777) : (CELL_FS_S_IFREG | 0666);
    if (m.read_only) mode &= ~0222;
    const s32 id = m.no_uid_gid ? -1 : 0;
    const u64 size = dir && dir_size_is_block ? m.block_size : static_cast<u64>(st.st_size);
    write_stat(g, sb, mode, id, id, st.st_atime, st.st_mtime, st.st_ctime, size, m.block_size);
}

void write_image_stat(const Guest& g, u32 sb, const Mount& m, const VfsImage::Stat& st, bool dir_size_is_block)
{
    s32 mode = st.dir ? (CELL_FS_S_IFDIR | 0777) : (CELL_FS_S_IFREG | 0666);
    if (m.read_only) mode &= ~0222;
    const s32 id = m.no_uid_gid ? -1 : 0;
    const u64 size = st.dir && dir_size_is_block ? m.block_size : st.size;
    write_stat(g, sb, mode, id, id, 0, 0, 0, size, m.block_size); // an image has no host times
}

bool image_is_dir(const Vfs::Resolved& res)
{
    const auto st = res.mount->image->stat(res.image_path);
    return st && st->dir;
}

std::shared_ptr<FsFile> get_file(Kernel& k, u32 fd)
{
    return std::dynamic_pointer_cast<FsFile>(k.impl().fds.find(FsObject::kIdBase, fd));
}

std::shared_ptr<FsDir> get_dir(Kernel& k, u32 fd)
{
    return std::dynamic_pointer_cast<FsDir>(k.impl().fds.find(FsObject::kIdBase, fd));
}

bool host_is_dir(const std::string& path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool host_exists(const std::string& path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

// Reads up to `nbytes` into guest `buf` (already validated) at `at`, or at the file position (advancing it) when `at`
// is empty. Host I/O and image reads go through a bounce buffer. `total` gets the bytes read; the file's lock is held.
Result read_locked(Kernel::Impl& s, FsFile& file, u32 buf, u64 nbytes, std::optional<u64> at, u64& total)
{
    std::vector<u8> bounce(static_cast<usize>(std::min<u64>(nbytes, kBounce)));
    total = 0;
    if (file.image_file) {
        const u64 file_size = file.image_file->size();
        u64 pos = at ? *at : file.pos;
        while (total < nbytes && pos < file_size) {
            const usize want = static_cast<usize>(std::min<u64>({nbytes - total, bounce.size(), file_size - pos}));
            if (!file.image_file->read(pos, bounce.data(), want)) {
                PS_LOG_ERROR(kFs, "read {}: reading the disc image failed at {:#x}", file.vpath, pos);
                return err(CELL_EIO);
            }
            std::memcpy(s.guest.ptr(buf + static_cast<u32>(total)), bounce.data(), want);
            total += want;
            pos += want;
        }
        if (!at) file.pos = pos;
        return CELL_OK;
    }
    while (total < nbytes) {
        const usize want = static_cast<usize>(std::min<u64>(nbytes - total, bounce.size()));
        const ssize_t n = at ? ::pread(file.host_fd, bounce.data(), want, static_cast<off_t>(*at + total))
                             : ::read(file.host_fd, bounce.data(), want);
        if (n < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno, "read", file.vpath);
        }
        if (n == 0) break;
        std::memcpy(s.guest.ptr(buf + static_cast<u32>(total)), bounce.data(), static_cast<usize>(n));
        total += static_cast<u64>(n);
    }
    return CELL_OK;
}

// Writes `nbytes` from guest `buf` (already validated) at `at`, or at the file position (the end with O_APPEND)
// when `at` is empty. Host files only: image mounts are read-only. The file's lock is held.
Result write_locked(Kernel::Impl& s, FsFile& file, u32 buf, u64 nbytes, std::optional<u64> at, u64& total)
{
    std::vector<u8> bounce(static_cast<usize>(std::min<u64>(nbytes, kBounce)));
    total = 0;
    if (!at && (file.flags & CELL_FS_O_APPEND)) ::lseek(file.host_fd, 0, SEEK_END);
    while (total < nbytes) {
        const usize chunk = static_cast<usize>(std::min<u64>(nbytes - total, bounce.size()));
        std::memcpy(bounce.data(), s.guest.ptr(buf + static_cast<u32>(total)), chunk);
        usize done = 0;
        while (done < chunk) {
            const ssize_t n = at ? ::pwrite(file.host_fd, bounce.data() + done, chunk - done,
                                       static_cast<off_t>(*at + total + done))
                                 : ::write(file.host_fd, bounce.data() + done, chunk - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                total += done;
                return map_errno(errno, "write", file.vpath);
            }
            done += static_cast<usize>(n);
        }
        total += chunk;
    }
    return CELL_OK;
}

// ". " and ".." first, as on the PS3, then the entries sorted by name (RPCS3 sys_fs_opendir); then the descriptor.
Result finish_opendir(Kernel::Impl& s, std::shared_ptr<FsDir> dir, u32 fd)
{
    std::sort(dir->entries.begin(), dir->entries.end(), [](const FsDir::Entry& a, const FsDir::Entry& b) { return a.name < b.name; });
    dir->entries.insert(dir->entries.begin(), {{".", true}, {"..", true}});
    dir->refs = 1;
    const u32 id = s.fds.insert(FsObject::kIdBase, dir);
    if (!id) return err(CELL_EMFILE);
    dir->id = id;
    s.guest.w32(fd, id);
    return CELL_OK;
}

// Gives an opened file its descriptor (lowest free, 3..254) and writes it to the guest.
Result install_file(Kernel::Impl& s, std::shared_ptr<FsFile> file, u32 fd)
{
    file->refs = 1;
    const u32 id = s.fds.insert(FsObject::kIdBase, file);
    if (!id) {
        PS_LOG_ERROR(kFs, "sys_fs_open({}): out of file descriptors", file->vpath);
        return err(CELL_EMFILE);
    }
    file->id = id;
    s.guest.w32(fd, id);
    PS_LOG_DEBUG(kFs, "sys_fs_open({}, {:#o}) -> fd {}", file->vpath, file->flags, id);
    return CELL_OK;
}

// RPCS3 verify_mself: magic "MSF\0", format version 1, 0x40-byte entries.
bool is_mself(const u8 (&header)[0x40])
{
    return ps_ld32(header) == 0x4D534600u && ps_ld32(header + 4) == 1 && ps_ld32(header + 20) == 0x40;
}

// A host file seen as an image file, so an NPD opener can read it (D-165). Owns the descriptor.
class HostFdFile final : public VfsImage::File {
public:
    HostFdFile(int fd, u64 size) : fd_(fd), size_(size) {}
    ~HostFdFile() override { ::close(fd_); }
    u64 size() const override { return size_; }
    bool read(u64 offset, void* dst, usize size) const override
    {
        usize done = 0;
        while (done < size) {
            const ssize_t n = ::pread(fd_, static_cast<u8*>(dst) + done, size - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += static_cast<usize>(n);
        }
        return true;
    }

private:
    int fd_;
    u64 size_;
};

// sys_fs_open's 8-byte type argument: SDATA (0x18000000010) or EDATA (2), else 0 (RPCS3 sys_fs_open).
u64 npd_type(Kernel::Impl& s, u32 arg, u64 size)
{
    if (size != 8 || !s.guest.valid(arg, 8)) return 0;
    const u64 type = s.guest.r64(arg);
    return type == 0x18000000010ull || type == 2 ? type : 0;
}

// An SDATA or EDATA file that starts "NPD\0", through the host's opener: the plain file, or the error to return.
Result open_npd(Kernel::Impl& s, std::shared_ptr<const VfsImage::File> encrypted, u64 type, s32 flags,
    std::string_view vpath, std::shared_ptr<const VfsImage::File>& plain)
{
    const char* kind = type == 2 ? "EDATA" : "SDATA";
    if ((flags & CELL_FS_O_ACCMODE) != CELL_FS_O_RDONLY) {
        PS_LOG_ERROR(kFs, "sys_fs_open({}): an encrypted {} file opened for writing ({:#o})", vpath, kind, flags);
        return err(CELL_EPERM);
    }
    Kernel::NpdOpener opener;
    {
        std::lock_guard lock(s.npd_mutex);
        opener = s.npd_opener;
    }
    std::string error = "no NPD opener in this process";
    if (opener) plain = opener(std::move(encrypted), type == 2, error);
    if (!plain) {
        PS_LOG_ERROR(kFs, "sys_fs_open({}): the encrypted {} file does not open: {}", vpath, kind, error);
        return err(CELL_EFSSPECIFIC);
    }
    PS_LOG_INFO(kFs, "sys_fs_open({}): encrypted {} file, {} bytes plain", vpath, kind, plain->size());
    return CELL_OK;
}

// sys_fs_open on an image mount. The mount is read-only, so this follows the host path's read-only rules: creating,
// truncating or opening for writing is CELL_EPERM, a missing file CELL_ENOENT.
Result open_image_file(Kernel::Impl& s, const Vfs::Resolved& res, const std::optional<VfsImage::Stat>& st, s32 flags,
    s32 mode, u32 arg, u64 size, u32 fd)
{
    if (!st) return err((flags & CELL_FS_O_CREAT) ? CELL_EPERM : CELL_ENOENT);
    if ((flags & CELL_FS_O_ACCMODE) != CELL_FS_O_RDONLY || (flags & CELL_FS_O_TRUNC)) return err(CELL_EPERM);
    auto image_file = res.mount->image->open(res.image_path);
    if (!image_file) {
        PS_LOG_ERROR(kFs, "sys_fs_open({}): the image lists the file but can't open it", res.vpath);
        return err(CELL_EIO);
    }
    if (flags & CELL_FS_O_MSELF) {
        u8 header[0x40]{};
        if (image_file->size() < sizeof(header) || !image_file->read(0, header, sizeof(header)) || !is_mself(header)) {
            return err(CELL_ENOTMSELF);
        }
    }
    if (const u64 type = npd_type(s, arg, size)) {
        u8 magic[4]{};
        if (image_file->size() >= 4 && image_file->read(0, magic, 4) && std::memcmp(magic, "NPD\0", 4) == 0) {
            std::shared_ptr<const VfsImage::File> plain;
            if (Result r = open_npd(s, std::move(image_file), type, flags, res.vpath, plain); r != CELL_OK) return r;
            image_file = std::move(plain);
        }
    }
    auto file = std::make_shared<FsFile>();
    file->vpath = res.vpath;
    file->mount = res.mount_ref;
    file->image_file = std::move(image_file);
    file->flags = flags;
    file->mode = mode;
    return install_file(s, std::move(file), fd);
}

} // namespace

// ------------------------------------------------------------------------------------------------- Vfs

bool Vfs::mount(std::string_view point, std::string_view host_dir, bool read_only)
{
    if (point.size() < 2 || point[0] != '/' || point.find('/', 1) != std::string_view::npos) {
        PS_LOG_ERROR(kFs, "mount point '{}' must be a single top-level name like /dev_hdd0", point);
        return false;
    }
    std::string host(host_dir);
    while (host.size() > 1 && host.back() == '/') host.pop_back();
    if (!host_is_dir(host)) {
        PS_LOG_ERROR(kFs, "mount {}: host directory '{}' does not exist", point, host);
        return false;
    }
    auto m = std::make_shared<Mount>();
    m->point = std::string(point);
    m->host = std::move(host);
    m->read_only = read_only;
    mount_properties(*m);
    if (point == "/dev_bdvd" && !read_only) PS_LOG_WARN(kFs, "/dev_bdvd mounted writable");
    PS_LOG_INFO(kFs, "mounted {} -> {}{}", m->point, m->host, read_only ? " (read-only)" : "");
    std::lock_guard lock(mutex_);
    mounts_[m->point] = std::move(m);
    return true;
}

bool Vfs::mount_image(std::string_view point, std::shared_ptr<const VfsImage> image, std::string_view subdir)
{
    if (point.size() < 2 || point[0] != '/' || point.find('/', 1) != std::string_view::npos || !image) {
        PS_LOG_ERROR(kFs, "mount point '{}' must be a single top-level name like /dev_bdvd, with an image", point);
        return false;
    }
    std::string root(subdir);
    while (!root.empty() && root.front() == '/') root.erase(0, 1);
    while (!root.empty() && root.back() == '/') root.pop_back();
    if (const auto st = image->stat(root); !st || !st->dir) {
        PS_LOG_ERROR(kFs, "mount {}: '{}' is not a directory of the image", point, root);
        return false;
    }
    auto m = std::make_shared<Mount>();
    m->point = std::string(point);
    m->image = std::move(image);
    m->image_root = std::move(root);
    m->read_only = true;
    mount_properties(*m);
    PS_LOG_INFO(kFs, "mounted {} -> disc image{}{} (read-only)", m->point, m->image_root.empty() ? "" : " /",
        m->image_root);
    std::lock_guard lock(mutex_);
    mounts_[m->point] = std::move(m);
    return true;
}

void Vfs::unmount(std::string_view point)
{
    std::lock_guard lock(mutex_);
    if (const auto it = mounts_.find(point); it != mounts_.end()) mounts_.erase(it);
}

std::vector<std::string> Vfs::mount_points() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [point, m] : mounts_) out.push_back(point);
    return out;
}

Vfs::Resolved Vfs::resolve(std::string_view vpath) const
{
    Resolved r;
    std::vector<std::string_view> parts;
    usize i = 0;
    std::string_view last;
    while (i < vpath.size()) {
        while (i < vpath.size() && vpath[i] == '/') i++;
        if (i >= vpath.size()) break;
        const usize end = std::min(vpath.find('/', i), vpath.size());
        const std::string_view part = vpath.substr(i, end - i);
        i = end;
        last = part;
        if (part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    r.trailing_dot = last == "." || last == "..";
    r.vpath = "/";
    for (usize k = 0; k < parts.size(); k++) {
        if (k) r.vpath += '/';
        r.vpath += parts[k];
    }
    if (parts.empty()) {
        r.root = true;
        return r;
    }
    std::shared_ptr<Mount> m;
    {
        std::lock_guard lock(mutex_);
        const auto it = mounts_.find("/" + std::string(parts[0]));
        if (it == mounts_.end()) return r;
        m = it->second;
    }
    r.mount_ref = m;
    r.mount = m.get();
    if (m->image) {
        r.image_path = m->image_root;
        for (usize k = 1; k < parts.size(); k++) {
            if (!r.image_path.empty()) r.image_path += '/';
            r.image_path += parts[k];
        }
        return r;
    }
    r.host = m->host;
    for (usize k = 1; k < parts.size(); k++) {
        r.host += '/';
        r.host += parts[k];
    }
    return r;
}

// ------------------------------------------------------------------------------------------------- syscalls

// PSPOCKET_HIDE_FILES=PATH[,PATH...]: those guest paths do not exist for open and stat (dev only: does a game get past
// a point without a file, a video it crashes in, say).
static bool hidden(std::string_view vpath)
{
    static const std::vector<std::string> paths = [] {
        std::vector<std::string> v;
        if (const char* e = std::getenv("PSPOCKET_HIDE_FILES")) {
            std::string all(e);
            for (size_t at = 0; at <= all.size();) {
                const size_t comma = all.find(',', at);
                const std::string one = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) v.push_back(one);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        return v;
    }();
    return std::find(paths.begin(), paths.end(), vpath) != paths.end();
}

Result sys_fs_open(PpuThread& t, u32 path, s32 flags, u32 fd, s32 mode, u32 arg, u64 size)
{
    Kernel& k = t.kernel;
    Kernel::Impl& s = k.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    if (!s.guest.valid(fd, 4)) return err(CELL_EFAULT);
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (hidden(res.vpath)) return err(CELL_ENOENT);
    if (res.root) return err(CELL_EISDIR);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    const Mount& m = *res.mount;

    std::optional<VfsImage::Stat> image_stat;
    if (m.image) image_stat = m.image->stat(res.image_path);
    if (m.image ? image_stat && image_stat->dir : host_is_dir(res.host)) return err(CELL_EISDIR);
    const s32 access = flags & CELL_FS_O_ACCMODE;
    if (access == CELL_FS_O_ACCMODE ||
        (flags & ~(CELL_FS_O_UNK | CELL_FS_O_ACCMODE | CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_APPEND |
                      CELL_FS_O_EXCL | CELL_FS_O_MSELF)) ||
        ((flags & CELL_FS_O_MSELF) && (flags & ~(CELL_FS_O_MSELF | CELL_FS_O_RDONLY)))) {
        PS_LOG_ERROR(kFs, "sys_fs_open({}): invalid or unsupported flags {:#o}", p.vpath, flags);
        return err(CELL_EINVAL);
    }

    if (m.image) return open_image_file(s, res, image_stat, flags, mode, arg, size, fd);

    const bool writable_fs = !m.read_only && has_write_rights(k, res.vpath);
    if (m.read_only && access != CELL_FS_O_RDONLY && host_exists(res.host)) return err(CELL_EPERM);

    int oflags = access == CELL_FS_O_WRONLY ? O_WRONLY : access == CELL_FS_O_RDWR ? O_RDWR : O_RDONLY;
    if ((flags & CELL_FS_O_CREAT) && writable_fs) {
        oflags |= O_CREAT;
        if (flags & CELL_FS_O_EXCL) oflags |= O_EXCL;
    }
    if ((flags & CELL_FS_O_TRUNC) && writable_fs) oflags |= O_TRUNC;
    if ((flags & CELL_FS_O_MSELF) || !writable_fs) oflags = O_RDONLY; // mutating flags are dropped (RPCS3)

    const int host_fd = ::open(res.host.c_str(), oflags | O_CLOEXEC, 0666);
    if (host_fd < 0) {
        const int e = errno;
        if (!writable_fs && (flags & CELL_FS_O_CREAT)) return err(m.read_only ? CELL_EPERM : CELL_EACCES);
        return map_errno(e, "sys_fs_open", p.vpath);
    }
    auto close_fail = [&](Result r) {
        ::close(host_fd);
        return r;
    };
    if ((flags & CELL_FS_O_TRUNC) && !writable_fs) return close_fail(err(m.read_only ? CELL_EPERM : CELL_EACCES));

    if (flags & CELL_FS_O_MSELF) {
        u8 header[0x40]{};
        const bool ok = ::pread(host_fd, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)) &&
            is_mself(header);
        if (!ok) return close_fail(err(CELL_ENOTMSELF));
    }
    if (const u64 type = npd_type(s, arg, size)) {
        u8 magic[4]{};
        struct stat st {};
        if (::pread(host_fd, magic, 4, 0) == 4 && std::memcmp(magic, "NPD\0", 4) == 0 && ::fstat(host_fd, &st) == 0) {
            // The descriptor goes to the encrypted file's reader: the open file reads its plain bytes.
            std::shared_ptr<const VfsImage::File> plain;
            const Result r = open_npd(s, std::make_shared<HostFdFile>(host_fd, static_cast<u64>(st.st_size)), type,
                flags, p.vpath, plain);
            if (r != CELL_OK) return r;
            auto file = std::make_shared<FsFile>();
            file->vpath = res.vpath;
            file->mount = res.mount_ref;
            file->image_file = std::move(plain);
            file->flags = flags;
            file->mode = mode;
            return install_file(s, std::move(file), fd);
        }
    }

    auto file = std::make_shared<FsFile>();
    file->vpath = res.vpath;
    file->mount = res.mount_ref;
    file->host_fd = host_fd;
    file->flags = flags;
    file->mode = mode;
    return install_file(s, std::move(file), fd); // on failure the FsFile destructor closes host_fd
}

Result sys_fs_read(PpuThread& t, u32 fd, u32 buf, u64 nbytes, u32 nread)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(nread, 8)) return err(CELL_EFAULT);
    if (!buf) {
        s.guest.w64(nread, 0);
        return err(CELL_EFAULT);
    }
    const auto file = get_file(t.kernel, fd);
    if (!file || (nbytes && (file->flags & CELL_FS_O_WRONLY))) {
        s.guest.w64(nread, 0);
        return err(CELL_EBADF);
    }
    if (!nbytes) {
        s.guest.w64(nread, 0);
        return CELL_OK;
    }
    if (!s.guest.valid(buf, nbytes)) {
        s.guest.w64(nread, 0);
        return err(CELL_EFAULT);
    }
    u64 total = 0;
    Result r = CELL_OK;
    {
        std::lock_guard lock(file->lock);
        if (!file->is_open()) return err(CELL_EBADF);
        r = read_locked(s, *file, buf, nbytes, std::nullopt, total);
    }
    s.guest.w64(nread, total);
    return r;
}

Result sys_fs_write(PpuThread& t, u32 fd, u32 buf, u64 nbytes, u32 nwrite)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(nwrite, 8)) return err(CELL_EFAULT);
    if (!buf) {
        s.guest.w64(nwrite, 0);
        return err(CELL_EFAULT);
    }
    const auto file = get_file(t.kernel, fd);
    if (!file || (nbytes && !(file->flags & CELL_FS_O_ACCMODE))) {
        s.guest.w64(nwrite, 0);
        return err(CELL_EBADF);
    }
    if (!nbytes) {
        s.guest.w64(nwrite, 0);
        return CELL_OK;
    }
    if (file->mount->read_only) {
        s.guest.w64(nwrite, 0);
        return err(CELL_EROFS);
    }
    if (!s.guest.valid(buf, nbytes)) {
        s.guest.w64(nwrite, 0);
        return err(CELL_EFAULT);
    }
    u64 total = 0;
    Result r = CELL_OK;
    {
        std::lock_guard lock(file->lock);
        if (file->host_fd < 0) return err(CELL_EBADF);
        r = write_locked(s, *file, buf, nbytes, std::nullopt, total);
    }
    s.guest.w64(nwrite, total);
    return r;
}

Result sys_fs_close(PpuThread& t, u32 fd)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto file = get_file(t.kernel, fd);
    if (!file) return err(CELL_EBADF);
    {
        std::lock_guard lock(file->lock);
        if (!file->is_open()) return err(CELL_EBADF);
        if (file->image_file) {
            file->image_file.reset();
        } else {
            if (file->flags & CELL_FS_O_ACCMODE) ::fsync(file->host_fd); // RPCS3 syncs written files on close
            ::close(file->host_fd);
            file->host_fd = -1;
        }
    }
    s.fds.remove(FsObject::kIdBase, fd);
    PS_LOG_DEBUG(kFs, "sys_fs_close(fd {})", fd);
    return CELL_OK;
}

Result sys_fs_opendir(PpuThread& t, u32 path, u32 fd)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    if (!s.guest.valid(fd, 4)) return err(CELL_EFAULT);
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);

    auto dir = std::make_shared<FsDir>();
    dir->vpath = res.vpath;
    if (res.root) {
        for (const std::string& point : s.vfs.mount_points()) dir->entries.push_back({point.substr(1), true});
    } else {
        if (!res.mount) return err(CELL_ENOTMOUNTED);
        dir->mount = res.mount_ref;
        if (res.mount->image) {
            const auto st = res.mount->image->stat(res.image_path);
            if (!st) return err(CELL_ENOENT);
            if (!st->dir) return err(CELL_ENOTDIR);
            const auto list = res.mount->image->list(res.image_path);
            if (!list) return err(CELL_EIO);
            for (const auto& [name, entry] : *list) dir->entries.push_back({name, entry.dir});
            return finish_opendir(s, std::move(dir), fd);
        }
        struct stat st {};
        if (::stat(res.host.c_str(), &st) != 0) return map_errno(errno, "sys_fs_opendir", p.vpath);
        if (!S_ISDIR(st.st_mode)) return err(CELL_ENOTDIR);
        DIR* d = ::opendir(res.host.c_str());
        if (!d) return map_errno(errno, "sys_fs_opendir", p.vpath);
        while (const dirent* e = ::readdir(d)) {
            const std::string_view name = e->d_name;
            if (name == "." || name == "..") continue;
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) is_dir = host_is_dir(res.host + "/" + std::string(name));
            dir->entries.push_back({std::string(name), is_dir});
        }
        ::closedir(d);
    }
    return finish_opendir(s, std::move(dir), fd);
}

Result sys_fs_readdir(PpuThread& t, u32 fd, u32 dir, u32 nread)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(dir, kCellFsDirentSize) || !s.guest.valid(nread, 8)) return err(CELL_EFAULT);
    const auto d = get_dir(t.kernel, fd);
    if (!d) return err(CELL_EBADF);
    FsDir::Entry entry;
    bool more = false;
    {
        std::lock_guard lock(d->lock);
        more = d->pos < d->entries.size();
        // Past the end lv2 still writes, polling the last entry (RPCS3 sys_fs_readdir).
        entry = more ? d->entries[d->pos++] : d->entries.back();
    }
    u8 out[kCellFsDirentSize]{};
    const usize len = std::min(entry.name.size(), kMaxName);
    out[0] = entry.dir ? CELL_FS_TYPE_DIRECTORY : CELL_FS_TYPE_REGULAR;
    out[1] = static_cast<u8>(len);
    std::memcpy(out + 2, entry.name.data(), len);
    if (!more && !(d->mount && d->mount->point == "/dev_hdd1")) {
        out[0] = 0; // the first three bytes are cleared at the end (except on dev_hdd1)
        out[1] = 0;
        out[2] = 0;
    }
    std::memcpy(s.guest.ptr(dir), out, sizeof(out));
    s.guest.w64(nread, more ? kCellFsDirentSize : 0);
    return CELL_OK;
}

Result sys_fs_closedir(PpuThread& t, u32 fd)
{
    if (!get_dir(t.kernel, fd)) return err(CELL_EBADF);
    if (!t.kernel.impl().fds.remove(FsObject::kIdBase, fd)) return err(CELL_EBADF);
    return CELL_OK;
}

Result sys_fs_stat(PpuThread& t, u32 path, u32 sb)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    if (!s.guest.valid(sb, kCellFsStatSize)) return err(CELL_EFAULT);
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (hidden(res.vpath)) return err(CELL_ENOENT);
    if (res.root) {
        write_stat(s.guest, sb, CELL_FS_S_IFDIR | 0711, -1, -1, -1, -1, -1, 258, 512); // RPCS3: the root ("/")
        return CELL_OK;
    }
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->image) {
        const auto st = res.mount->image->stat(res.image_path);
        if (!st) return err(CELL_ENOENT);
        if (res.trailing_dot && !st->dir) return err(CELL_ENOTDIR);
        write_image_stat(s.guest, sb, *res.mount, *st, true);
        return CELL_OK;
    }
    struct stat st {};
    if (::stat(res.host.c_str(), &st) != 0) return map_errno(errno, "sys_fs_stat", p.vpath);
    if (res.trailing_dot && !S_ISDIR(st.st_mode)) return err(CELL_ENOTDIR);
    write_host_stat(s.guest, sb, *res.mount, st, true);
    return CELL_OK;
}

Result sys_fs_fstat(PpuThread& t, u32 fd, u32 sb)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto file = get_file(t.kernel, fd);
    if (!file) return err(CELL_EBADF);
    if (!s.guest.valid(sb, kCellFsStatSize)) return err(CELL_EFAULT);
    struct stat st {};
    {
        std::lock_guard lock(file->lock);
        if (!file->is_open()) return err(CELL_EBADF);
        if (file->image_file) {
            write_image_stat(s.guest, sb, *file->mount, {false, file->image_file->size()}, false);
            return CELL_OK;
        }
        if (::fstat(file->host_fd, &st) != 0) return map_errno(errno, "sys_fs_fstat", file->vpath);
    }
    write_host_stat(s.guest, sb, *file->mount, st, false);
    return CELL_OK;
}

Result sys_fs_mkdir(PpuThread& t, u32 path, s32)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root) return err(CELL_EEXIST);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->read_only) return err(CELL_EROFS);
    if (!host_exists(res.host) && !has_write_rights(t.kernel, res.vpath)) return err(CELL_EACCES);
    if (::mkdir(res.host.c_str(), 0777) != 0) return map_errno(errno, "sys_fs_mkdir", p.vpath);
    return CELL_OK;
}

Result sys_fs_rename(PpuThread& t, u32 from, u32 to)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg pf = read_path(s.guest, from);
    if (pf.error) return pf.error;
    const PathArg pt = read_path(s.guest, to);
    if (pt.error) return pt.error;
    const Vfs::Resolved rf = s.vfs.resolve(pf.vpath);
    const Vfs::Resolved rt = s.vfs.resolve(pt.vpath);
    if (rf.root || rt.root) return err(CELL_EPERM);
    if (!rf.mount || !rt.mount) return err(CELL_ENOTMOUNTED);
    if (rf.mount != rt.mount) return err(CELL_EXDEV);
    if (rf.mount->read_only) return err(CELL_EROFS);
    // lv2 does not replace an existing target (RPCS3 vfs::host::rename with overwrite = false).
#if defined(__APPLE__)
    if (::renamex_np(rf.host.c_str(), rt.host.c_str(), RENAME_EXCL) != 0) return map_errno(errno, "sys_fs_rename", pf.vpath);
#else
    if (host_exists(rt.host)) return err(CELL_EEXIST);
    if (::rename(rf.host.c_str(), rt.host.c_str()) != 0) return map_errno(errno, "sys_fs_rename", pf.vpath);
#endif
    return CELL_OK;
}

Result sys_fs_rmdir(PpuThread& t, u32 path)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root) return err(CELL_EPERM);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->read_only) return err(CELL_EROFS);
    if (host_is_dir(res.host) && !has_write_rights(t.kernel, res.vpath)) return err(CELL_EACCES);
    if (::rmdir(res.host.c_str()) != 0) {
        const int e = errno;
        return e == EEXIST ? err(CELL_ENOTEMPTY) : map_errno(e, "sys_fs_rmdir", p.vpath);
    }
    return CELL_OK;
}

Result sys_fs_unlink(PpuThread& t, u32 path)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root) return err(CELL_EISDIR);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->image ? image_is_dir(res) : host_is_dir(res.host)) return err(CELL_EISDIR);
    if (res.mount->read_only) return err(CELL_EROFS);
    if (::unlink(res.host.c_str()) != 0) return map_errno(errno, "sys_fs_unlink", p.vpath);
    return CELL_OK;
}

Result sys_fs_lseek(PpuThread& t, u32 fd, s64 offset, s32 whence, u32 pos)
{
    Kernel::Impl& s = t.kernel.impl();
    const auto file = get_file(t.kernel, fd);
    if (!file) return err(CELL_EBADF);
    if (static_cast<u32>(whence) >= 3) return err(CELL_EINVAL);
    if (!s.guest.valid(pos, 8)) return err(CELL_EFAULT);
    off_t result = 0;
    {
        std::lock_guard lock(file->lock);
        if (!file->is_open()) return err(CELL_EBADF);
        if (file->image_file) {
            const s64 from = whence == CELL_FS_SEEK_SET ? 0
                : whence == CELL_FS_SEEK_CUR        ? static_cast<s64>(file->pos)
                                                    : static_cast<s64>(file->image_file->size());
            s64 to = 0;
            if (__builtin_add_overflow(from, offset, &to) || to < 0) return err(CELL_EINVAL);
            file->pos = static_cast<u64>(to);
            s.guest.w64(pos, file->pos);
            return CELL_OK;
        }
        const int host_whence = whence == CELL_FS_SEEK_SET ? SEEK_SET : whence == CELL_FS_SEEK_CUR ? SEEK_CUR : SEEK_END;
        result = ::lseek(file->host_fd, static_cast<off_t>(offset), host_whence);
    }
    if (result < 0) return err(CELL_EINVAL);
    s.guest.w64(pos, static_cast<u64>(result));
    return CELL_OK;
}

static Result sync_file(PpuThread& t, u32 fd)
{
    const auto file = get_file(t.kernel, fd);
    if (!file || !(file->flags & CELL_FS_O_ACCMODE)) return err(CELL_EBADF);
    std::lock_guard lock(file->lock);
    if (file->host_fd < 0) return err(CELL_EBADF);
    ::fsync(file->host_fd);
    return CELL_OK;
}

Result sys_fs_fsync(PpuThread& t, u32 fd) { return sync_file(t, fd); }

Result sys_fs_fdatasync(PpuThread& t, u32 fd) { return sync_file(t, fd); }

Result sys_fs_truncate(PpuThread& t, u32 path, u64 size)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root) return err(CELL_EISDIR);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->read_only) return err(CELL_EROFS);
    if (::truncate(res.host.c_str(), static_cast<off_t>(size)) != 0) return map_errno(errno, "sys_fs_truncate", p.vpath);
    return CELL_OK;
}

Result sys_fs_ftruncate(PpuThread& t, u32 fd, u64 size)
{
    const auto file = get_file(t.kernel, fd);
    if (!file || !(file->flags & CELL_FS_O_ACCMODE)) return err(CELL_EBADF);
    if (file->mount->read_only) return err(CELL_EROFS);
    std::lock_guard lock(file->lock);
    if (file->host_fd < 0) return err(CELL_EBADF);
    if (::ftruncate(file->host_fd, static_cast<off_t>(size)) != 0) return map_errno(errno, "sys_fs_ftruncate", file->vpath);
    return CELL_OK;
}

// RPCS3 sys_fs_chmod: lv2 checks the path and changes nothing (the PS3 file systems keep no permissions games see).
Result sys_fs_chmod(PpuThread& t, u32 path, s32)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root || !res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->read_only) return err(CELL_EROFS);
    struct stat st {};
    if (::stat(res.host.c_str(), &st) != 0) return map_errno(errno, "sys_fs_chmod", p.vpath);
    return CELL_OK;
}

// RPCS3 sys_fs_utime. CellFsUtimbuf: actime, modtime (s64 each, 4-byte aligned).
Result sys_fs_utime(PpuThread& t, u32 path, u32 timep)
{
    Kernel::Impl& s = t.kernel.impl();
    const PathArg p = read_path(s.guest, path);
    if (p.error) return p.error;
    if (!s.guest.valid(timep, 16)) return err(CELL_EFAULT);
    const Vfs::Resolved res = s.vfs.resolve(p.vpath);
    if (res.root) return err(CELL_EISDIR);
    if (!res.mount) return err(CELL_ENOTMOUNTED);
    if (res.mount->read_only) return err(CELL_EROFS);
    struct timeval times[2] {};
    times[0].tv_sec = static_cast<time_t>(static_cast<s64>(s.guest.r64(timep)));
    times[1].tv_sec = static_cast<time_t>(static_cast<s64>(s.guest.r64(timep + 8)));
    if (::utimes(res.host.c_str(), times) != 0) return map_errno(errno, "sys_fs_utime", p.vpath);
    return CELL_OK;
}

// RPCS3 sys_fs_fcntl, for the operations cellFs reaches it with:
// - 0x8000000A / 0x8000000B (cellFsReadWithOffset / cellFsWriteWithOffset): read or write at an offset without
//   moving the file position. lv2_file_op_rw (0x38 bytes): vtable, op, 2 x u32, fd, buf, offset (u64), size (u64),
//   out_code (s32) at 0x28, out_size (u64) at 0x30.
// - 0xE0000017 (cellFsAllocateFileAreaWithoutZeroFill): sets a file's size. lv2_file_e0000017 (0x28 bytes): size,
//   0x10, 0x20, -, path, size (u64) at 0x18, out_code at 0x20.
// Deviation: any other operation logs an error and returns CELL_EINVAL (RPCS3 logs and returns CELL_OK), so nothing
// silently succeeds (CLAUDE.md rule 4).
Result sys_fs_fcntl(PpuThread& t, u32 fd, u32 op, u32 arg, u32 size)
{
    Kernel::Impl& s = t.kernel.impl();
    switch (op) {
    case 0x8000000A:
    case 0x8000000B: {
        const bool write = op == 0x8000000B;
        if (size < 0x38) return err(CELL_EINVAL);
        if (!s.guest.valid(arg, 0x38)) return err(CELL_EFAULT);
        const auto file = get_file(t.kernel, fd);
        if (!file) return err(CELL_EBADF);
        if (!write && (file->flags & CELL_FS_O_WRONLY)) return err(CELL_EBADF);
        if (write && (!(file->flags & CELL_FS_O_ACCMODE) || (file->flags & CELL_FS_O_APPEND))) return err(CELL_EBADF);
        if (write && file->mount->read_only) return err(CELL_EROFS);
        const u32 buf = s.guest.r32(arg + 0x14);
        const u64 offset = s.guest.r64(arg + 0x18);
        const u64 nbytes = s.guest.r64(arg + 0x20);
        if (nbytes && !s.guest.valid(buf, nbytes)) return err(CELL_EFAULT);
        u64 done = 0;
        Result r = CELL_OK;
        {
            std::lock_guard lock(file->lock);
            if (!file->is_open()) return err(CELL_EBADF);
            if (nbytes) {
                r = write ? write_locked(s, *file, buf, nbytes, offset, done)
                          : read_locked(s, *file, buf, nbytes, offset, done);
            }
        }
        s.guest.w64(arg + 0x30, done);
        s.guest.w32(arg + 0x28, static_cast<u32>(r));
        return CELL_OK;
    }
    case 0xE0000017: {
        if (!s.guest.valid(arg, 0x28)) return err(CELL_EFAULT);
        if (size < s.guest.r32(arg) || s.guest.r32(arg + 4) != 0x10 || s.guest.r32(arg + 8) != 0x20) {
            return err(CELL_EINVAL);
        }
        const Result r = sys_fs_truncate(t, s.guest.r32(arg + 0x10), s.guest.r64(arg + 0x18));
        s.guest.w32(arg + 0x20, static_cast<u32>(r));
        return CELL_OK;
    }
    default:
        // RPCS3 logs an unknown operation and returns CELL_OK (D-166); its 31 no-op operations land here too.
        PS_LOG_ERROR(kFs, "sys_fs_fcntl(fd {}, op {:#x}, size {:#x}): operation not implemented; CELL_OK as RPCS3", fd,
            op, size);
        return CELL_OK;
    }
}

void register_fs_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 801, sys_fs_open);
    PS_LV2_BIND(table, 802, sys_fs_read);
    PS_LV2_BIND(table, 803, sys_fs_write);
    PS_LV2_BIND(table, 804, sys_fs_close);
    PS_LV2_BIND(table, 805, sys_fs_opendir);
    PS_LV2_BIND(table, 806, sys_fs_readdir);
    PS_LV2_BIND(table, 807, sys_fs_closedir);
    PS_LV2_BIND(table, 808, sys_fs_stat);
    PS_LV2_BIND(table, 809, sys_fs_fstat);
    PS_LV2_BIND(table, 811, sys_fs_mkdir);
    PS_LV2_BIND(table, 812, sys_fs_rename);
    PS_LV2_BIND(table, 813, sys_fs_rmdir);
    PS_LV2_BIND(table, 814, sys_fs_unlink);
    PS_LV2_BIND(table, 815, sys_fs_utime);
    PS_LV2_BIND(table, 817, sys_fs_fcntl);
    PS_LV2_BIND(table, 818, sys_fs_lseek);
    PS_LV2_BIND(table, 819, sys_fs_fdatasync);
    PS_LV2_BIND(table, 820, sys_fs_fsync);
    PS_LV2_BIND(table, 831, sys_fs_truncate);
    PS_LV2_BIND(table, 832, sys_fs_ftruncate);
    PS_LV2_BIND(table, 834, sys_fs_chmod);
}

} // namespace pspocket::lv2
