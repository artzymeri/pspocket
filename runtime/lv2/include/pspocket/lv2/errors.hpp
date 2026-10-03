#pragma once

// lv2 result codes (CELL_E*), from RPCS3 rpcs3/Emu/Cell/ErrorCodes.h. Syscalls return them in r3 sign-extended
// to 64 bits, as RPCS3's error_code (an s32) does: CELL_ESRCH reads 0xFFFFFFFF80010005 in r3.

#include "pspocket/core/types.hpp"

namespace pspocket::lv2 {

enum CellError : u32 {
    CELL_EAGAIN = 0x80010001,       // resource temporarily unavailable
    CELL_EINVAL = 0x80010002,       // invalid argument
    CELL_ENOSYS = 0x80010003,       // not implemented
    CELL_ENOMEM = 0x80010004,       // memory allocation failure
    CELL_ESRCH = 0x80010005,        // no object with that ID
    CELL_ENOENT = 0x80010006,       // file does not exist
    CELL_ENOEXEC = 0x80010007,      // unrecognized file format
    CELL_EDEADLK = 0x80010008,      // deadlock avoided
    CELL_EPERM = 0x80010009,        // operation not permitted
    CELL_EBUSY = 0x8001000A,        // device or resource busy
    CELL_ETIMEDOUT = 0x8001000B,    // timed out
    CELL_EABORT = 0x8001000C,       // aborted
    CELL_EFAULT = 0x8001000D,       // invalid memory access
    CELL_ENOCHILD = 0x8001000E,     // no child process
    CELL_ESTAT = 0x8001000F,        // invalid thread state
    CELL_EALIGN = 0x80010010,       // invalid alignment
    CELL_EKRESOURCE = 0x80010011,   // kernel resources exhausted
    CELL_EISDIR = 0x80010012,       // is a directory
    CELL_ECANCELED = 0x80010013,    // canceled
    CELL_EEXIST = 0x80010014,       // already exists
    CELL_EISCONN = 0x80010015,      // already connected
    CELL_ENOTCONN = 0x80010016,     // not connected
    CELL_EAUTHFAIL = 0x80010017,    // program authentication failed
    CELL_ENOTMSELF = 0x80010018,    // not an MSELF
    CELL_ESYSVER = 0x80010019,      // system version error
    CELL_EAUTHFATAL = 0x8001001A,   // fatal system error
    CELL_EDOM = 0x8001001B,         // math domain
    CELL_ERANGE = 0x8001001C,       // math range
    CELL_EILSEQ = 0x8001001D,       // illegal multi-byte sequence
    CELL_EFPOS = 0x8001001E,        // file position error
    CELL_EINTR = 0x8001001F,        // interrupted
    CELL_EFBIG = 0x80010020,        // file too large
    CELL_EMLINK = 0x80010021,       // too many links
    CELL_ENFILE = 0x80010022,       // file table overflow
    CELL_ENOSPC = 0x80010023,       // no space left on device
    CELL_ENOTTY = 0x80010024,       // not a TTY
    CELL_EPIPE = 0x80010025,        // broken pipe
    CELL_EROFS = 0x80010026,        // read-only file system
    CELL_ESPIPE = 0x80010027,       // illegal seek
    CELL_E2BIG = 0x80010028,        // argument list too long
    CELL_EACCES = 0x80010029,       // access denied
    CELL_EBADF = 0x8001002A,        // bad file descriptor
    CELL_EIO = 0x8001002B,          // I/O error
    CELL_EMFILE = 0x8001002C,       // too many open files
    CELL_ENODEV = 0x8001002D,       // no device
    CELL_ENOTDIR = 0x8001002E,      // not a directory
    CELL_ENXIO = 0x8001002F,        // no such device or address
    CELL_EXDEV = 0x80010030,        // cross-device link
    CELL_EBADMSG = 0x80010031,      // bad message
    CELL_EINPROGRESS = 0x80010032,  // in progress
    CELL_EMSGSIZE = 0x80010033,     // message size
    CELL_ENAMETOOLONG = 0x80010034, // name too long
    CELL_ENOLCK = 0x80010035,       // no lock
    CELL_ENOTEMPTY = 0x80010036,    // directory not empty
    CELL_ENOTSUP = 0x80010037,      // not supported
    CELL_EFSSPECIFIC = 0x80010038,  // file-system specific error
    CELL_EOVERFLOW = 0x80010039,    // overflow
    CELL_ENOTMOUNTED = 0x8001003A,  // file system not mounted
    CELL_ENOTSDATA = 0x8001003B,    // not SDATA
    CELL_ESDKVER = 0x8001003C,      // SDK version error
    CELL_ENOLICDISC = 0x8001003D,
    CELL_ENOLICENT = 0x8001003E,
};

constexpr s32 CELL_OK = 0;
constexpr s32 CELL_CANCEL = 1; // not an error (sys_ppu_thread_yield)

// Result of a kernel operation: CELL_OK, a CellError, or (for a few syscalls) a non-negative value.
using Result = s32;

constexpr Result err(CellError e) { return static_cast<Result>(e); }

// The r3 value for a result: sign-extended, as RPCS3 stores error_code (s32) into the 64-bit GPR.
constexpr u64 to_r3(Result r) { return static_cast<u64>(static_cast<s64>(r)); }

const char* error_name(Result r);

} // namespace pspocket::lv2
