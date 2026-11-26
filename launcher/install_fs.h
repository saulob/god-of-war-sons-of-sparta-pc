// File operations of the game installer. On Windows paths reach Win32 in extended-length form, so staging files past
// MAX_PATH work even when LongPathsEnabled is off (longPathAware alone does not lift the limit then).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace sos::installfs {

// Includes the terminating NUL: 259 usable characters.
inline constexpr size_t kMaxPath = 260;

enum class FsProblem { kNone, kOther, kPathTooLong, kAccessDenied, kInUse, kDiskFull, kInvalidName, kNotFound };

struct FsError {
    std::string operation; // empty: no error
    std::filesystem::path path;
    std::filesystem::path target;
    std::error_code code;
    unsigned long win32 = 0;

    bool failed() const { return !operation.empty(); }
    size_t path_length() const;
    FsProblem problem() const;
    std::string Describe() const;
};

std::filesystem::path Extended(const std::filesystem::path& path);
std::filesystem::path Plain(const std::filesystem::path& path);
// UTF-16 units on Windows, as MAX_PATH counts them.
size_t PathLength(const std::filesystem::path& path);
// RtlAreLongPathsEnabled: LongPathsEnabled is on and the manifest is longPathAware.
bool ProcessLongPathsEnabled();
const char* Win32ErrorName(unsigned long code);

// Tests only: an error the hook returns fails that operation.
using FaultHook = std::function<std::error_code(std::string_view operation, const std::filesystem::path& path)>;
void SetFaultHookForTesting(FaultHook hook);

bool CreateDirectories(const std::filesystem::path& dir, FsError& error);
bool Rename(const std::filesystem::path& from, const std::filesystem::path& to, FsError& error);
bool Remove(const std::filesystem::path& path, FsError& error);
bool RemoveAll(const std::filesystem::path& path, FsError& error);
bool Exists(const std::filesystem::path& path);
bool IsDirectory(const std::filesystem::path& path);
bool IsRegularFile(const std::filesystem::path& path);
bool IsEmptyDirectory(const std::filesystem::path& path);
bool FileSize(const std::filesystem::path& path, uint64_t& size, FsError& error);
bool SpaceAvailable(const std::filesystem::path& dir, uint64_t& available, FsError& error);
bool HardLink(const std::filesystem::path& existing, const std::filesystem::path& link, FsError& error);

enum class CopyOutcome { kCopied, kCancelled, kFailed };
CopyOutcome CopyFileTo(const std::filesystem::path& from, const std::filesystem::path& to,
                       const std::function<bool(uint64_t copied)>& progress, FsError& error);

struct ListedFile {
    std::filesystem::path relative;
    uint64_t size = 0;
};
bool ListFiles(const std::filesystem::path& dir, std::vector<ListedFile>& files, FsError& error,
               const std::function<bool(const std::filesystem::path& name)>& skip = {});

bool OpenForRead(const std::filesystem::path& path, std::ifstream& in, FsError& error);

class OutputFile {
public:
    OutputFile() = default;
    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    bool Open(const std::filesystem::path& path, FsError& error);
    bool Write(const void* data, size_t size, FsError& error);
    bool Close(FsError& error);

private:
    std::filesystem::path path_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    std::FILE* file_ = nullptr;
#endif
};

bool WriteWholeFile(const std::filesystem::path& path, const void* data, size_t size, FsError& error);

} // namespace sos::installfs
