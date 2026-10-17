// kernel32: files, directories and paths on top of POSIX, through the VFS.
// Overlapped I/O completes synchronously (pread/pwrite at the requested offset),
// which Windows allows and the game's streaming code handles.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "../shim.h"
#include "../vfs.h"

using namespace shim;

namespace {

struct File : KObject {
    File() : KObject(Kind::File) {}
    ~File() override {
        if (fd >= 0 && fd > 2) ::close(fd);
    }
    int fd = -1;
    bool console = false;
    bool overlapped = false;  // opened with FILE_FLAG_OVERLAPPED
    std::string win, host;
    std::mutex mu;  // serialises seek+read/write pairs
};

struct Find : KObject {
    Find() : KObject(Kind::Find) {}
    std::vector<std::string> names;
    std::string host_dir;
    size_t next = 0;
};

constexpr uint32_t GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr uint32_t CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;
constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x1, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr uint32_t INVALID_ATTRS = 0xFFFFFFFF;

uint32_t errno_to_win(int e) {
    switch (e) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EEXIST: return ERROR_FILE_EXISTS;
        case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
        default: return ERROR_INVALID_PARAMETER;
    }
}

std::string parent(const std::string& host) {
    const size_t s = host.rfind('/');
    return s == std::string::npos ? "." : host.substr(0, s);
}

void mkdirs(const std::string& dir) {
    struct stat st;
    if (dir.empty() || stat(dir.c_str(), &st) == 0) return;
    mkdirs(parent(dir));
    ::mkdir(dir.c_str(), 0755);
}

bool is_data_path(const std::string& host) { return host.rfind(vfs::data_dir(), 0) == 0; }

std::shared_ptr<File> std_handle(uint32_t which) {
    static std::shared_ptr<File> out[3];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 3; ++i) {
            out[i] = std::make_shared<File>();
            out[i]->fd = i;
            out[i]->console = true;
        }
    });
    return out[which];
}

std::shared_ptr<File> file_for(uint32_t h) {
    if (h == 0xFFFFFFF6) return std_handle(0);  // STD_INPUT_HANDLE values used as handles
    if (h == 0xFFFFFFF5) return std_handle(1);
    if (h == 0xFFFFFFF4) return std_handle(2);
    return handle_as<File>(h);
}

}  // namespace

uint32_t shim::open_file(const std::string& win, uint32_t access, uint32_t disposition) {
    return open_file_ex(win, access, disposition, 0);
}

uint32_t shim::open_file_ex(const std::string& win, uint32_t access, uint32_t disposition, uint32_t flags_attrs) {
    const std::string host = vfs::host_path(win);
    int flags = O_RDONLY;
    if ((access & GENERIC_WRITE) && (access & GENERIC_READ)) flags = O_RDWR;
    else if (access & GENERIC_WRITE) flags = O_WRONLY;
    switch (disposition) {
        case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
        default: break;
    }
    if (flags & O_CREAT) {
        if (is_data_path(host)) mkdirs(parent(host));
        vfs::invalidate(parent(host));
    }
    struct stat st;
    const bool existed = stat(host.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) flags = O_RDONLY;  // FILE_FLAG_BACKUP_SEMANTICS directory handles
    const int fd = ::open(host.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_last_error(errno_to_win(errno) == ERROR_FILE_NOT_FOUND && !existed && errno == ENOENT &&
                               stat(parent(host).c_str(), &st) != 0
                           ? ERROR_PATH_NOT_FOUND
                           : errno_to_win(errno));
        return kInvalidHandle;
    }
    auto f = std::make_shared<File>();
    f->fd = fd;
    f->win = vfs::full_path(win);
    f->host = host;
    f->overlapped = (flags_attrs & 0x40000000) != 0;
    set_last_error((existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0);
    static const bool log_files = std::getenv("DIRT2_LOG_FILES") != nullptr;
    if (log_files) rt::log("t%u open %s%s", rt::current_thread_id(), f->win.c_str(), f->overlapped ? " (overlapped)" : "");
    return handle_new(f);
}

namespace {

void filetime_from(uint32_t dst, const timespec& ts) {
    const uint64_t ft = (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
    wr64(dst, ft);
}

uint32_t attrs_of(const struct stat& st) {
    uint32_t a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

uint32_t get_attrs(const std::string& win) {
    struct stat st;
    if (stat(vfs::host_path(win).c_str(), &st) != 0) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_ATTRS;
    }
    return attrs_of(st);
}

// Completes an OVERLAPPED request: Internal, InternalHigh, then signals hEvent.
void complete_overlapped(uint32_t ov, uint32_t bytes) {
    wr32(ov + 0, 0);  // STATUS_SUCCESS
    wr32(ov + 4, bytes);
    if (const uint32_t ev = rd32(ov + 16)) set_event(ev);
}

uint32_t do_io(uint32_t h, uint32_t buf, uint32_t n, uint32_t done_out, uint32_t ov, bool write) {
    auto f = file_for(h);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    ssize_t r;
    if (ov) {
        const off_t off = off_t(rd32(ov + 8)) | (off_t(rd32(ov + 12)) << 32);
        r = write ? ::pwrite(f->fd, rt::host_ptr(buf), n, off) : ::pread(f->fd, rt::host_ptr(buf), n, off);
    } else {
        std::lock_guard lk(f->mu);
        r = write ? ::write(f->fd, rt::host_ptr(buf), n) : ::read(f->fd, rt::host_ptr(buf), n);
    }
    static const bool log_io = std::getenv("DIRT2_LOG_FILES") != nullptr;
    if (log_io && !ov)
        rt::log("t%u %s %s n=%u -> %zd", rt::current_thread_id(), write ? "write" : "read", f->win.c_str(), n, r);
    if (log_io && ov)
        rt::log("t%u %s %s @%llu n=%u -> %zd (ov %08X ev %08X)", rt::current_thread_id(), write ? "write" : "read",
                f->win.c_str(), (unsigned long long)(off_t(rd32(ov + 8)) | (off_t(rd32(ov + 12)) << 32)), n, r, ov,
                rd32(ov + 16));
    if (r < 0) {
        set_last_error(errno_to_win(errno));
        if (done_out) wr32(done_out, 0);
        return 0;
    }
    if (done_out) wr32(done_out, uint32_t(r));
    if (ov) {
        if (!write && r == 0 && n) {
            complete_overlapped(ov, 0);
            wr32(ov + 0, 0xC0000011);  // STATUS_END_OF_FILE
            set_last_error(ERROR_HANDLE_EOF);
            return 0;
        }
        complete_overlapped(ov, uint32_t(r));
        // Like Windows for real asynchronous handles: report the request as
        // pending even though the data is already there; the caller then
        // completes it through its event / GetOverlappedResult path.
        if (f->overlapped) {
            if (done_out) wr32(done_out, 0);
            set_last_error(ERROR_IO_PENDING);
            return 0;
        }
    }
    return 1;
}

void fill_find_data(uint32_t d, const std::string& dir, const std::string& name, bool wide) {
    std::memset(rt::host_ptr(d), 0, wide ? 592 : 320);
    struct stat st{};
    stat((dir + "/" + name).c_str(), &st);
    wr32(d + 0, attrs_of(st));
    filetime_from(d + 4, st.st_ctimespec);
    filetime_from(d + 12, st.st_atimespec);
    filetime_from(d + 20, st.st_mtimespec);
    wr32(d + 28, uint32_t(uint64_t(st.st_size) >> 32));
    wr32(d + 32, uint32_t(st.st_size));
    if (wide) copy_out_w(d + 44, 260, name);
    else copy_out(d + 44, 260, name);
}

uint32_t find_first(const std::string& pattern_win, uint32_t data, bool wide) {
    const std::string full = vfs::full_path(pattern_win);
    const size_t s = full.rfind('\\');
    const std::string dir_win = full.substr(0, s), pat = full.substr(s + 1);
    auto f = std::make_shared<Find>();
    f->host_dir = vfs::host_path(dir_win.size() == 2 ? dir_win + "\\" : dir_win);
    if (DIR* d = opendir(f->host_dir.c_str())) {
        while (dirent* e = readdir(d))
            if (fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD) == 0 || pat == "*.*" || pat == "*") f->names.emplace_back(e->d_name);
        closedir(d);
    }
    if (f->names.empty()) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
    }
    fill_find_data(data, f->host_dir, f->names[0], wide);
    f->next = 1;
    return handle_new(f);
}

uint32_t find_next(uint32_t h, uint32_t data, bool wide) {
    auto f = handle_as<Find>(h);
    if (!f || f->next >= f->names.size()) {
        set_last_error(ERROR_NO_MORE_FILES);
        return 0;
    }
    fill_find_data(data, f->host_dir, f->names[f->next++], wide);
    return 1;
}

uint32_t full_path_out(const std::string& in, uint32_t cap, uint32_t buf, uint32_t file_part, bool wide) {
    const std::string full = vfs::full_path(in);
    const uint32_t need = uint32_t(wide ? to_utf16(full).size() : full.size());
    if (cap <= need) return need + 1;
    if (wide) copy_out_w(buf, cap, full);
    else copy_out(buf, cap, full);
    if (file_part) {
        const size_t s = full.rfind('\\');
        const uint32_t off = uint32_t(wide ? to_utf16(full.substr(0, s + 1)).size() * 2 : s + 1);
        wr32(file_part, buf + off);
    }
    return need;
}

}  // namespace

STDCALL(kernel32, CreateFileA, 7) {
    const uint32_t h = open_file_ex(a.str(0), a[1], a[4], a[5]);
    if (h == kInvalidHandle) rt::log("CreateFileA(%s) failed: %u", a.str(0), last_error());
    return h;
}
STDCALL(kernel32, CreateFileW, 7) {
    const std::string n = from_utf16(a[0]);
    const uint32_t h = open_file_ex(n, a[1], a[4], a[5]);
    if (h == kInvalidHandle) rt::log("CreateFileW(%s) failed: %u", n.c_str(), last_error());
    return h;
}
STDCALL(kernel32, ReadFile, 5) { return do_io(a[0], a[1], a[2], a[3], a[4], false); }
STDCALL(kernel32, WriteFile, 5) { return do_io(a[0], a[1], a[2], a[3], a[4], true); }
STDCALL(kernel32, GetOverlappedResult, 4) {  // (file, ov, *bytes, wait)
    if (a[2]) wr32(a[2], rd32(a[1] + 4));
    const uint32_t status = rd32(a[1]);
    if (status == 0xC0000011) {  // STATUS_END_OF_FILE
        set_last_error(ERROR_HANDLE_EOF);
        return 0;
    }
    return 1;
}
STDCALL(kernel32, CancelIo, 1) { return 1; }
STDCALL(kernel32, FlushFileBuffers, 1) { return 1; }

STDCALL(kernel32, SetFilePointer, 4) {
    auto f = file_for(a[0]);
    if (!f) return 0xFFFFFFFF;
    int64_t off = int32_t(a[1]);
    if (a[2]) off = int64_t((uint64_t(rd32(a[2])) << 32) | a[1]);
    std::lock_guard lk(f->mu);
    const off_t r = ::lseek(f->fd, off, int(a[3]));  // FILE_BEGIN/CURRENT/END match SEEK_*
    if (r < 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFF;
    }
    if (a[2]) wr32(a[2], uint32_t(uint64_t(r) >> 32));
    set_last_error(0);
    return uint32_t(r);
}
STDCALL(kernel32, SetFilePointerEx, 5) {
    auto f = file_for(a[0]);
    if (!f) return 0;
    const int64_t off = int64_t((uint64_t(a[2]) << 32) | a[1]);
    std::lock_guard lk(f->mu);
    const off_t r = ::lseek(f->fd, off, int(a[4]));
    if (r < 0) return 0;
    if (a[3]) wr64(a[3], uint64_t(r));
    return 1;
}
STDCALL(kernel32, GetFileSizeEx, 2) {
    auto f = file_for(a[0]);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return 0;
    wr64(a[1], uint64_t(st.st_size));
    return 1;
}
STDCALL(kernel32, SetEndOfFile, 1) {
    auto f = file_for(a[0]);
    if (!f) return 0;
    std::lock_guard lk(f->mu);
    return ::ftruncate(f->fd, ::lseek(f->fd, 0, SEEK_CUR)) == 0 ? 1 : 0;
}
STDCALL(kernel32, GetFileType, 1) {
    auto f = file_for(a[0]);
    if (!f) return 0;
    return f->console ? 2 : 1;  // FILE_TYPE_CHAR / FILE_TYPE_DISK
}
STDCALL(kernel32, GetFileInformationByHandle, 2) {
    auto f = file_for(a[0]);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return 0;
    const uint32_t d = a[1];
    std::memset(rt::host_ptr(d), 0, 52);
    wr32(d + 0, attrs_of(st));
    filetime_from(d + 4, st.st_ctimespec);
    filetime_from(d + 12, st.st_atimespec);
    filetime_from(d + 20, st.st_mtimespec);
    wr32(d + 28, 0x1234);  // volume serial
    wr32(d + 32, uint32_t(uint64_t(st.st_size) >> 32));
    wr32(d + 36, uint32_t(st.st_size));
    wr32(d + 40, 1);
    wr32(d + 44, uint32_t(st.st_ino >> 32));
    wr32(d + 48, uint32_t(st.st_ino));
    return 1;
}
STDCALL(kernel32, GetStdHandle, 1) { return a[0]; }  // the STD_* values double as handles
STDCALL(kernel32, SetStdHandle, 2) { return 1; }
STDCALL(kernel32, SetHandleCount, 1) { return a[0]; }

STDCALL(kernel32, GetFileAttributesA, 1) { return get_attrs(a.str(0)); }
STDCALL(kernel32, GetFileAttributesExA, 3) {
    struct stat st;
    if (stat(vfs::host_path(a.str(0)).c_str(), &st) != 0) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    const uint32_t d = a[2];
    wr32(d + 0, attrs_of(st));
    filetime_from(d + 4, st.st_ctimespec);
    filetime_from(d + 12, st.st_atimespec);
    filetime_from(d + 20, st.st_mtimespec);
    wr32(d + 28, uint32_t(uint64_t(st.st_size) >> 32));
    wr32(d + 32, uint32_t(st.st_size));
    return 1;
}
STDCALL(kernel32, SetFileAttributesA, 2) { return 1; }
STDCALL(kernel32, SetFileAttributesW, 2) { return 1; }

static uint32_t make_dir(const std::string& win) {
    const std::string host = vfs::host_path(win);
    if (is_data_path(host)) mkdirs(parent(host));
    vfs::invalidate(parent(host));
    if (::mkdir(host.c_str(), 0755) != 0) {
        set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
        return 0;
    }
    return 1;
}
STDCALL(kernel32, CreateDirectoryA, 2) { return make_dir(a.str(0)); }
STDCALL(kernel32, CreateDirectoryW, 2) { return make_dir(from_utf16(a[0])); }
STDCALL(kernel32, RemoveDirectoryA, 1) {
    const std::string host = vfs::host_path(a.str(0));
    vfs::invalidate(parent(host));
    return ::rmdir(host.c_str()) == 0 ? 1 : 0;
}
static uint32_t delete_file(const std::string& win) {
    const std::string host = vfs::host_path(win);
    vfs::invalidate(parent(host));
    if (::unlink(host.c_str()) != 0) {
        set_last_error(errno_to_win(errno));
        return 0;
    }
    return 1;
}
STDCALL(kernel32, DeleteFileA, 1) { return delete_file(a.str(0)); }
STDCALL(kernel32, DeleteFileW, 1) { return delete_file(from_utf16(a[0])); }
STDCALL(kernel32, MoveFileW, 2) {
    const std::string from = vfs::host_path(from_utf16(a[0])), to = vfs::host_path(from_utf16(a[1]));
    vfs::invalidate(parent(from));
    vfs::invalidate(parent(to));
    return ::rename(from.c_str(), to.c_str()) == 0 ? 1 : 0;
}

STDCALL(kernel32, FindFirstFileA, 2) { return find_first(a.str(0), a[1], false); }
STDCALL(kernel32, FindFirstFileW, 2) { return find_first(from_utf16(a[0]), a[1], true); }
STDCALL(kernel32, FindNextFileA, 2) { return find_next(a[0], a[1], false); }
STDCALL(kernel32, FindNextFileW, 2) { return find_next(a[0], a[1], true); }
STDCALL(kernel32, FindClose, 1) { return handle_close(a[0]) ? 1 : 0; }

STDCALL(kernel32, GetFullPathNameA, 4) { return full_path_out(a.str(0), a[1], a[2], a[3], false); }
STDCALL(kernel32, GetFullPathNameW, 4) { return full_path_out(from_utf16(a[0]), a[1], a[2], a[3], true); }
STDCALL(kernel32, GetCurrentDirectoryA, 2) {
    const std::string d = vfs::cwd();
    if (a[0] <= d.size()) return uint32_t(d.size() + 1);
    return copy_out(a[1], a[0], d);
}
STDCALL(kernel32, SetCurrentDirectoryA, 1) {
    if (vfs::set_cwd(a.str(0))) return 1;
    set_last_error(ERROR_PATH_NOT_FOUND);
    return 0;
}
STDCALL(kernel32, GetDriveTypeA, 1) { return 3; }  // DRIVE_FIXED
STDCALL(kernel32, GetDriveTypeW, 1) { return 3; }
STDCALL(kernel32, GetDiskFreeSpaceA, 5) {
    if (a[1]) wr32(a[1], 8);
    if (a[2]) wr32(a[2], 512);
    if (a[3]) wr32(a[3], 0x00F00000);
    if (a[4]) wr32(a[4], 0x01000000);
    return 1;
}
STDCALL(kernel32, GetDiskFreeSpaceExA, 4) {
    const uint64_t free = 64ull << 30, total = 256ull << 30;
    if (a[1]) wr64(a[1], free);
    if (a[2]) wr64(a[2], total);
    if (a[3]) wr64(a[3], free);
    return 1;
}
STDCALL(kernel32, GetWindowsDirectoryA, 2) { return copy_out(a[0], a[1], "C:\\Windows"); }
STDCALL(kernel32, GetSystemDirectoryA, 2) { return copy_out(a[0], a[1], "C:\\Windows\\system32"); }

// Pipes and child processes are only used by optional tools paths.
STDCALL(kernel32, CreatePipe, 4) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
STDCALL(kernel32, PeekNamedPipe, 6) { return 0; }

// Console output goes to stderr.
static uint32_t console_write(uint32_t buf, uint32_t n, uint32_t written, bool wide) {
    const std::string s = wide ? from_utf16(buf, int(n)) : std::string(rt::host_str(buf), n);
    std::fwrite(s.data(), 1, s.size(), stderr);
    if (written) wr32(written, n);
    return 1;
}
STDCALL(kernel32, WriteConsoleA, 5) { return console_write(a[1], a[2], a[3], false); }
STDCALL(kernel32, WriteConsoleW, 5) { return console_write(a[1], a[2], a[3], true); }
STDCALL(kernel32, GetConsoleMode, 2) { return 0; }
STDCALL(kernel32, GetConsoleCP, 0) { return 437; }
STDCALL(kernel32, GetConsoleOutputCP, 0) { return 437; }
STDCALL(kernel32, SetConsoleCtrlHandler, 2) { return 1; }
