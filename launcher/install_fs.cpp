#include "install_fs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <vector>

#include "utf8.h"

namespace sos::installfs {
namespace {

namespace fs = std::filesystem;

FaultHook& Hook() {
    static FaultHook hook;
    return hook;
}

#ifdef _WIN32
// The Win32 error a std::error_code carries: libc++'s filesystem reports them in the system category.
unsigned long Win32Of(const std::error_code& code) {
    return code && code.category() == std::system_category() ? static_cast<unsigned long>(code.value()) : 0;
}

std::error_code LastError(DWORD fallback = ERROR_GEN_FAILURE) {
    const DWORD error = GetLastError();
    return {static_cast<int>(error ? error : fallback), std::system_category()};
}
#else
unsigned long Win32Of(const std::error_code&) { return 0; }

std::error_code LastErrno() {
    return {errno ? errno : EIO, std::generic_category()};
}
#endif

bool Fail(FsError& error, const char* operation, const fs::path& path, std::error_code code,
          const fs::path& target = {}) {
    error.operation = operation;
    error.path = Plain(path);
    error.target = Plain(target);
    error.code = code;
    error.win32 = Win32Of(code);
    return false;
}

bool Injected(FsError& error, const char* operation, const fs::path& path, const fs::path& target = {}) {
    if (!Hook()) {
        return false;
    }
    std::error_code code = Hook()(operation, Plain(path));
    if (!code && !target.empty()) {
        code = Hook()(std::string(operation) + " target", Plain(target));
    }
    if (!code) {
        return false;
    }
    Fail(error, operation, path, code, target);
    return true;
}

std::string OneLine(std::string text) {
    for (char& c : text) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

} // namespace

size_t FsError::path_length() const {
    return path.empty() ? 0 : PathLength(path);
}

FsProblem FsError::problem() const {
    if (!failed()) {
        return FsProblem::kNone;
    }
    switch (win32) {
    case 0: break;
    case 206: return FsProblem::kPathTooLong;           // ERROR_FILENAME_EXCED_RANGE
    case 3:                                             // ERROR_PATH_NOT_FOUND: also a path too long for Win32
        return path_length() >= kMaxPath ? FsProblem::kPathTooLong : FsProblem::kNotFound;
    case 2: return FsProblem::kNotFound;
    case 5: case 19: return FsProblem::kAccessDenied;   // ERROR_ACCESS_DENIED, ERROR_WRITE_PROTECT
    case 32: case 33: case 1224: return FsProblem::kInUse; // sharing and lock violations, a mapped file
    case 39: case 112: return FsProblem::kDiskFull;     // ERROR_HANDLE_DISK_FULL, ERROR_DISK_FULL
    case 123: case 161: return FsProblem::kInvalidName; // ERROR_INVALID_NAME, ERROR_BAD_PATHNAME
    default: return FsProblem::kOther;
    }
    if (code.category() == std::generic_category() || code.category() == std::system_category()) {
        switch (code.value()) {
        case ENAMETOOLONG: return FsProblem::kPathTooLong;
        case EACCES: case EPERM: case EROFS: return FsProblem::kAccessDenied;
        case EBUSY: case ETXTBSY: return FsProblem::kInUse;
        case ENOSPC:
#ifdef EDQUOT
        case EDQUOT:
#endif
            return FsProblem::kDiskFull;
        case ENOENT: return FsProblem::kNotFound;
        case EILSEQ: return FsProblem::kInvalidName;
        default: break;
        }
    }
    return FsProblem::kOther;
}

std::string FsError::Describe() const {
    if (!failed()) {
        return {};
    }
    std::string text = operation + " failed: path=" + Utf8(path);
    if (!target.empty()) {
        text += " target=" + Utf8(target);
    }
    text += " path_length=" + std::to_string(path_length());
    if (!target.empty()) {
        text += " target_length=" + std::to_string(PathLength(target));
    }
    text += " error=" + std::to_string(code.value()) + " category=" + code.category().name() + " message=" +
            OneLine(code.message());
    if (win32) {
        const char* name = Win32ErrorName(win32);
        text += " win32=" + std::to_string(win32) + (*name ? std::string(" ") + name : std::string());
    }
    return text;
}

fs::path Extended(const fs::path& path) {
#ifdef _WIN32
    if (path.empty()) {
        return path;
    }
    const std::wstring& given = path.native();
    const auto starts = [&](const wchar_t* prefix) {
        return given.size() >= 4 && std::equal(prefix, prefix + 4, given.begin(), [](wchar_t a, wchar_t b) {
                   return a == b || ((a == L'\\' || a == L'/') && (b == L'\\' || b == L'/'));
               });
    };
    if (starts(L"\\\\?\\") || starts(L"\\\\.\\")) {
        fs::path same = path;
        return same.make_preferred();
    }
    std::error_code ec;
    const fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) {
        return path;
    }
    std::wstring full = absolute.lexically_normal().make_preferred().native();
    while (full.size() > 3 && full.back() == L'\\') {
        full.pop_back();
    }
    if (full.size() >= 3 && std::iswalpha(full[0]) && full[1] == L':' && full[2] == L'\\') {
        return fs::path(L"\\\\?\\" + full);
    }
    if (full.size() > 2 && full[0] == L'\\' && full[1] == L'\\' && full[2] != L'\\') {
        return fs::path(L"\\\\?\\UNC\\" + full.substr(2));
    }
    return path;
#else
    return path;
#endif
}

fs::path Plain(const fs::path& path) {
#ifdef _WIN32
    const std::wstring& text = path.native();
    if (text.starts_with(L"\\\\?\\UNC\\")) {
        return fs::path(L"\\\\" + text.substr(8));
    }
    if (text.starts_with(L"\\\\?\\")) {
        return fs::path(text.substr(4));
    }
#endif
    return path;
}

size_t PathLength(const fs::path& path) {
#ifdef _WIN32
    return Plain(Extended(path)).native().size();
#else
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    return (ec ? path : absolute).native().size();
#endif
}

bool ProcessLongPathsEnabled() {
#ifdef _WIN32
    using Query = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<Query>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")) : nullptr;
    return query && query();
#else
    return true;
#endif
}

const char* Win32ErrorName(unsigned long code) {
    switch (code) {
    case 2: return "ERROR_FILE_NOT_FOUND";
    case 3: return "ERROR_PATH_NOT_FOUND";
    case 5: return "ERROR_ACCESS_DENIED";
    case 17: return "ERROR_NOT_SAME_DEVICE";
    case 19: return "ERROR_WRITE_PROTECT";
    case 21: return "ERROR_NOT_READY";
    case 32: return "ERROR_SHARING_VIOLATION";
    case 33: return "ERROR_LOCK_VIOLATION";
    case 39: return "ERROR_HANDLE_DISK_FULL";
    case 80: return "ERROR_FILE_EXISTS";
    case 82: return "ERROR_CANNOT_MAKE";
    case 112: return "ERROR_DISK_FULL";
    case 123: return "ERROR_INVALID_NAME";
    case 145: return "ERROR_DIR_NOT_EMPTY";
    case 161: return "ERROR_BAD_PATHNAME";
    case 183: return "ERROR_ALREADY_EXISTS";
    case 206: return "ERROR_FILENAME_EXCED_RANGE";
    case 225: return "ERROR_VIRUS_INFECTED";
    case 226: return "ERROR_VIRUS_DELETED";
    case 1224: return "ERROR_USER_MAPPED_FILE";
    case 1235: return "ERROR_REQUEST_ABORTED";
    case 1392: return "ERROR_FILE_CORRUPT";
    default: return "";
    }
}

void SetFaultHookForTesting(FaultHook hook) {
    Hook() = std::move(hook);
}

bool CreateDirectories(const fs::path& dir, FsError& error) {
    if (Injected(error, "create directory", dir)) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(Extended(dir), ec);
    return !ec || Fail(error, "create directory", dir, ec);
}

bool Rename(const fs::path& from, const fs::path& to, FsError& error) {
    if (Injected(error, "rename", from, to)) {
        return false;
    }
    std::error_code ec;
    fs::rename(Extended(from), Extended(to), ec);
    return !ec || Fail(error, "rename", from, ec, to);
}

bool Remove(const fs::path& path, FsError& error) {
    if (Injected(error, "remove", path)) {
        return false;
    }
    std::error_code ec;
    fs::remove(Extended(path), ec);
    return !ec || Fail(error, "remove", path, ec);
}

bool RemoveAll(const fs::path& path, FsError& error) {
    if (Injected(error, "remove folder", path)) {
        return false;
    }
    std::error_code ec;
    fs::remove_all(Extended(path), ec);
    return !ec || Fail(error, "remove folder", path, ec);
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(Extended(path), ec);
}

bool IsDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(Extended(path), ec);
}

bool IsRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(Extended(path), ec);
}

bool IsEmptyDirectory(const fs::path& path) {
    std::error_code ec;
    const bool empty = fs::is_empty(Extended(path), ec);
    return !ec && empty;
}

bool FileSize(const fs::path& path, uint64_t& size, FsError& error) {
    std::error_code ec;
    size = fs::file_size(Extended(path), ec);
    return !ec || Fail(error, "read file size", path, ec);
}

bool SpaceAvailable(const fs::path& dir, uint64_t& available, FsError& error) {
    std::error_code ec;
    const fs::space_info space = fs::space(Extended(dir), ec);
    available = ec ? 0 : space.available;
    return !ec || Fail(error, "read free space", dir, ec);
}

bool HardLink(const fs::path& existing, const fs::path& link, FsError& error) {
    if (Injected(error, "hard link", existing, link)) {
        return false;
    }
#ifdef _WIN32
    if (CreateHardLinkW(Extended(link).c_str(), Extended(existing).c_str(), nullptr)) {
        return true;
    }
    return Fail(error, "hard link", existing, LastError(), link);
#else
    std::error_code ec;
    fs::create_hard_link(existing, link, ec);
    return !ec || Fail(error, "hard link", existing, ec, link);
#endif
}

#ifdef _WIN32
namespace {
struct CopyProgressContext {
    const std::function<bool(uint64_t)>* progress;
    bool cancelled = false;
};

DWORD CALLBACK CopyProgressRoutine(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                   HANDLE, HANDLE, LPVOID data) {
    auto* context = static_cast<CopyProgressContext*>(data);
    if (*context->progress && !(*context->progress)(uint64_t(transferred.QuadPart))) {
        context->cancelled = true;
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}
} // namespace
#endif

CopyOutcome CopyFileTo(const fs::path& from, const fs::path& to, const std::function<bool(uint64_t)>& progress,
                     FsError& error) {
    if (Injected(error, "copy file", from, to)) {
        return CopyOutcome::kFailed;
    }
#ifdef _WIN32
    CopyProgressContext context{&progress};
    if (CopyFileExW(Extended(from).c_str(), Extended(to).c_str(), CopyProgressRoutine, &context, nullptr,
                    COPY_FILE_FAIL_IF_EXISTS)) {
        return CopyOutcome::kCopied;
    }
    const std::error_code code = LastError();
    if (context.cancelled) {
        return CopyOutcome::kCancelled;
    }
    Fail(error, "copy file", from, code, to);
    return CopyOutcome::kFailed;
#else
    std::ifstream in;
    if (!OpenForRead(from, in, error)) {
        return CopyOutcome::kFailed;
    }
    OutputFile out;
    if (!out.Open(to, error)) {
        return CopyOutcome::kFailed;
    }
    std::vector<char> block(1u << 20);
    uint64_t copied = 0;
    while (in.read(block.data(), std::streamsize(block.size())).gcount() > 0) {
        const size_t count = size_t(in.gcount());
        if (!out.Write(block.data(), count, error)) {
            return CopyOutcome::kFailed;
        }
        copied += count;
        if (progress && !progress(copied)) {
            return CopyOutcome::kCancelled;
        }
    }
    if (!in.eof()) {
        Fail(error, "read file", from, LastErrno());
        return CopyOutcome::kFailed;
    }
    return out.Close(error) ? CopyOutcome::kCopied : CopyOutcome::kFailed;
#endif
}

bool ListFiles(const fs::path& dir, std::vector<ListedFile>& files, FsError& error,
               const std::function<bool(const fs::path& name)>& skip) {
    if (Injected(error, "list folder", dir)) {
        return false;
    }
    const fs::path base = Extended(dir);
    std::error_code ec;
    fs::recursive_directory_iterator it(base, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (skip && skip(path.filename())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            ec.clear();
            continue;
        }
        const bool regular = it->is_regular_file(ec);
        if (ec) {
            return Fail(error, "read file type", path, ec);
        }
        if (!regular) {
            continue;
        }
        const uint64_t size = it->file_size(ec);
        if (ec) {
            return Fail(error, "read file size", path, ec);
        }
        files.push_back({path.lexically_relative(base), size});
    }
    return !ec || Fail(error, "list folder", dir, ec);
}

bool OpenForRead(const fs::path& path, std::ifstream& in, FsError& error) {
    if (Injected(error, "open file", path)) {
        return false;
    }
#ifdef _WIN32
    SetLastError(0);
#endif
    errno = 0;
    in.open(Extended(path), std::ios::binary);
    if (in) {
        return true;
    }
#ifdef _WIN32
    return Fail(error, "open file", path, LastError(ERROR_OPEN_FAILED));
#else
    return Fail(error, "open file", path, LastErrno());
#endif
}

OutputFile::~OutputFile() {
#ifdef _WIN32
    if (handle_) {
        CloseHandle(handle_);
    }
#else
    if (file_) {
        std::fclose(file_);
    }
#endif
}

bool OutputFile::Open(const fs::path& path, FsError& error) {
    path_ = path;
    if (Injected(error, "create file", path)) {
        return false;
    }
#ifdef _WIN32
    const HANDLE handle = CreateFileW(Extended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Fail(error, "create file", path, LastError());
    }
    handle_ = handle;
#else
    errno = 0;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        return Fail(error, "create file", path, LastErrno());
    }
#endif
    return true;
}

bool OutputFile::Write(const void* data, size_t size, FsError& error) {
    if (Injected(error, "write file", path_)) {
        return false;
    }
#ifdef _WIN32
    if (!handle_) {
        return Fail(error, "write file", path_, {ERROR_INVALID_HANDLE, std::system_category()});
    }
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle_, bytes, chunk, &written, nullptr) || written == 0) {
            return Fail(error, "write file", path_, LastError(ERROR_WRITE_FAULT));
        }
        bytes += written;
        size -= written;
    }
    return true;
#else
    if (!file_) {
        return Fail(error, "write file", path_, {EBADF, std::generic_category()});
    }
    errno = 0;
    if (size > 0 && std::fwrite(data, 1, size, file_) != size) {
        return Fail(error, "write file", path_, LastErrno());
    }
    return true;
#endif
}

bool OutputFile::Close(FsError& error) {
#ifdef _WIN32
    const HANDLE handle = handle_;
    handle_ = nullptr;
    if (handle && !CloseHandle(handle)) {
        return Fail(error, "write file", path_, LastError(ERROR_WRITE_FAULT));
    }
#else
    std::FILE* file = file_;
    file_ = nullptr;
    errno = 0;
    if (file && std::fclose(file) != 0) {
        return Fail(error, "write file", path_, LastErrno());
    }
#endif
    return true;
}

bool WriteWholeFile(const fs::path& path, const void* data, size_t size, FsError& error) {
    OutputFile file;
    return (!path.has_parent_path() || CreateDirectories(path.parent_path(), error)) && file.Open(path, error) &&
           file.Write(data, size, error) && file.Close(error);
}

} // namespace sos::installfs
