#include "game_install.h"

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
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "boot_image.h"
#include "build_diagnostics.h"
#include "install_fs.h"
#include "localization/ui_text.h"
#include "pkg_extract.h"
#include "utf8.h"

namespace sos {
namespace {

namespace fs = std::filesystem;
using installfs::FsError;
using installfs::FsProblem;

constexpr uint64_t kSpaceMargin = 256ull << 20;
constexpr int kRenameAttempts = 10;
constexpr char kCleanupPhase[] = "Removing the incomplete installation...";

InstallResult Failure(InstallStatus status, std::string detail = {}) {
    InstallResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

// A full disk is reported as one.
InstallResult FsFailure(InstallStatus status, const std::string& verb, FsError error) {
    if (status == InstallStatus::kWriteFailed && error.problem() == FsProblem::kDiskFull) {
        status = InstallStatus::kNoSpace;
    }
    InstallResult result = Failure(status, verb + " " + Utf8(error.path) + ": " + error.code.message());
    result.fs_error = std::move(error);
    return result;
}

fs::path WithSuffix(const fs::path& dir, const char* suffix) {
    return dir.parent_path() / (dir.filename().native() + PathFromUtf8(suffix).native());
}

bool IsWithin(const fs::path& child, const fs::path& parent) {
    const fs::path relative = child.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path Normal(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    return (ec ? path : absolute).lexically_normal();
}

// The antivirus scanner can hold a freshly written folder for a moment.
bool RenameWithRetry(const fs::path& from, const fs::path& to, FsError& error) {
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
        error = {};
        if (installfs::Rename(from, to, error)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

bool RenameWithRetry(const fs::path& from, const fs::path& to) {
    FsError ignored;
    return RenameWithRetry(from, to, ignored);
}

// Hard links need both paths on one volume: the drive on Windows, the device of the nearest existing folder elsewhere.
bool SameVolume(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return a.root_name() == b.root_name();
#else
    const auto device = [](fs::path path) {
        std::error_code ec;
        while (!path.empty() && !fs::exists(path, ec)) {
            path = path.parent_path();
        }
        struct stat info;
        return !path.empty() && stat(path.c_str(), &info) == 0 ? std::optional<dev_t>(info.st_dev) : std::nullopt;
    };
    const auto first = device(a), second = device(b);
    return first && second && *first == *second;
#endif
}

// An existing folder is only replaced when it is empty or already holds what the installer writes there.
bool IsReplaceable(const fs::path& dir, const char* own_file) {
    if (!installfs::Exists(dir)) {
        return true;
    }
    return installfs::IsDirectory(dir) && (installfs::IsEmptyDirectory(dir) || installfs::Exists(dir / own_file));
}

using Leftovers = std::vector<std::string>;

class StagingGuard {
public:
    StagingGuard(fs::path dir, const InstallHooks& hooks, Leftovers& leftovers)
        : dir_(std::move(dir)), hooks_(hooks), leftovers_(leftovers) {}
    ~StagingGuard() {
        if (!armed_ || !installfs::Exists(dir_)) {
            return;
        }
        if (hooks_.phase) {
            hooks_.phase(kCleanupPhase);
        }
        if (FsError error; !installfs::RemoveAll(dir_, error)) {
            leftovers_.push_back(error.Describe());
        }
    }
    void Release() { armed_ = false; }

private:
    fs::path dir_;
    const InstallHooks& hooks_;
    Leftovers& leftovers_;
    bool armed_ = true;
};

using SourceFile = installfs::ListedFile;

// Skipped as scripts/game_view.py skips them: extraction leftovers and an earlier preparation's marker.
bool IsSkipped(const fs::path& name) {
    const std::string text = Utf8(name);
    return text == ".pkg_extracted" || text == kPreparedMarker || (text.size() > 5 && text.ends_with(".part"));
}

std::string JsonQuote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

struct CopyContext {
    const InstallHooks* hooks;
    uint64_t done, total;
    bool cancelled;
};

enum class Transfer { kLinked, kCopied, kCancelled, kFailed };

// A hard link costs no space; another drive gets a copy.
Transfer TransferFile(const fs::path& from, const fs::path& to, CopyContext& copy, FsError& error) {
    if (FsError link_error; installfs::HardLink(from, to, link_error)) {
        return Transfer::kLinked;
    }
    const auto progress = [&copy](uint64_t copied) {
        if (copy.hooks->progress && !copy.hooks->progress(copy.done + copied, copy.total)) {
            copy.cancelled = true;
            return false;
        }
        return true;
    };
    switch (installfs::CopyFileTo(from, to, progress, error)) {
    case installfs::CopyOutcome::kCopied: return Transfer::kCopied;
    case installfs::CopyOutcome::kCancelled: return Transfer::kCancelled;
    case installfs::CopyOutcome::kFailed: break;
    }
    return copy.cancelled ? Transfer::kCancelled : Transfer::kFailed;
}

InstallStatus FromCheck(const GameCheck& check) {
    switch (check.state) {
    case GameFileState::kValid: return InstallStatus::kInstalled;
    case GameFileState::kMissing: return InstallStatus::kIncomplete;
    case GameFileState::kReadError: return InstallStatus::kReadFailed;
    case GameFileState::kDifferentVersion:
        return check.detail.starts_with("param.sfo TITLE_ID") || check.detail.starts_with("param.sfo CONTENT_ID")
                   ? InstallStatus::kWrongGame
                   : InstallStatus::kWrongVersion;
    }
    return InstallStatus::kIncomplete;
}

// A God of War: Sons of Sparta the port does not support is reported with its diagnostics; anything else keeps its failure.
InstallResult UnsupportedOrFailure(const GameCheck& check, const GameSource& source, const std::string& sfo) {
    if (check.state == GameFileState::kDifferentVersion) {
        UnsupportedReport report = DiagnoseUnsupportedBuild(source, sfo);
        if (report.recognized) {
            InstallResult result = Failure(InstallStatus::kUnsupportedBuild, check.detail);
            result.diagnostics = std::move(report.lines);
            return result;
        }
        if (!report.rejection.empty()) {
            return Failure(FromCheck(check), check.detail + "; " + report.rejection);
        }
    }
    return Failure(FromCheck(check), check.detail);
}

InstallResult UnsupportedOrFailure(const GameCheck& check, const fs::path& folder) {
    if (check.state != GameFileState::kDifferentVersion) {
        return Failure(FromCheck(check), check.detail);
    }
    const std::unique_ptr<GameSource> source = OpenFolderSource(folder);
    std::vector<uint8_t> sfo;
    source->Read("sce_sys/param.sfo", 1u << 20, sfo);
    return UnsupportedOrFailure(check, *source, std::string(sfo.begin(), sfo.end()));
}

// The folders the installer writes: the prepared game and the boot image, each with a staging and a backup folder.
struct Layout {
    explicit Layout(const GamePaths& target)
        : app0(Normal(target.app0)), image(Normal(target.image_dir)), app0_stage(WithSuffix(app0, ".install-temp")),
          app0_backup(WithSuffix(app0, ".install-backup")), image_stage(WithSuffix(image, ".install-temp")),
          image_backup(WithSuffix(image, ".install-backup")) {}

    bool Overlaps(const fs::path& source) const {
        for (const fs::path& own : {app0, app0_stage, app0_backup, image, image_stage, image_backup}) {
            if (source == own || IsWithin(source, own) || IsWithin(own, source)) {
                return true;
            }
        }
        return false;
    }

    fs::path app0, image, app0_stage, app0_backup, image_stage, image_backup;
};

// A source inside, or holding, the folders of any supported build is refused before it is read.
bool OverlapsAnyBuild(const fs::path& source, const GameRoots& roots, std::span<const GameBuild> builds) {
    for (const GameBuild& build : builds) {
        if (Layout(BuildPaths(roots, build)).Overlaps(source)) {
            return true;
        }
    }
    return false;
}

// Creates the parent folders, checks the free space and clears old staging folders.
InstallResult PrepareStaging(const Layout& layout, uint64_t needed) {
    for (const fs::path& dir : {layout.app0.parent_path(), layout.image.parent_path()}) {
        if (FsError error; !installfs::CreateDirectories(dir, error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot create", std::move(error));
        }
    }
    uint64_t available = 0;
    if (FsError error; installfs::SpaceAvailable(layout.app0.parent_path(), available, error) && needed &&
                       available < needed + kSpaceMargin) {
        return Failure(InstallStatus::kNoSpace,
                       "needs " + std::to_string(needed) + " bytes, " + std::to_string(available) + " available");
    }
    for (const fs::path& dir : {layout.app0_stage, layout.image_stage}) {
        FsError error;
        if (!installfs::RemoveAll(dir, error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot clear", std::move(error));
        }
        if (installfs::Exists(dir)) {
            return Failure(InstallStatus::kWriteFailed, "cannot clear " + Utf8(dir));
        }
    }
    return {};
}

// The marker scripts/game_view.py writes, so the development tools see an installed game as prepared. A marker that is
// a link into an installed game is removed, not written through.
bool WriteMarker(const fs::path& app0, const fs::path& source, size_t linked, size_t copied, const GameBuild& build,
                 const char* scope, FsError& error) {
    std::ostringstream marker;
    marker << "{\n  \"layers\": [\n    " << JsonQuote(Utf8(source)) << "\n  ],\n  \"description\": \""
           << build.title_id << ' ' << build.version << "\",\n  \"link\": " << linked << ",\n  \"copy\": " << copied
           << ",\n  \"replaced\": 0,\n  \"version\": \"" << build.version << "\",\n  \"scope\": \"" << scope << "\"\n}";
    const std::string text = marker.str();
    installfs::OutputFile file;
    return installfs::Remove(app0 / kPreparedMarker, error) && file.Open(app0 / kPreparedMarker, error) &&
           file.Write(text.data(), text.size(), error) && file.Close(error);
}

InstallResult MarkerFailure(FsError error) {
    return FsFailure(InstallStatus::kWriteFailed, "cannot write the preparation marker", std::move(error));
}

InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard);

// Builds the boot image from the staged game, checks both and swaps them in. Until the swap the current installation
// is untouched; a failed swap puts it back.
InstallResult BuildAndActivate(const Layout& layout, StagingGuard& app0_guard, StagingGuard& image_guard,
                               const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build,
                               uint64_t total) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    phase("Preparing the game...");
    if (const std::string error = hooks.build_image ? hooks.build_image(layout.app0_stage, layout.image_stage)
                                                    : BuildBootImage(layout.app0_stage, layout.image_stage);
        !error.empty()) {
        return Failure(InstallStatus::kPrepareFailed, error);
    }

    phase("Verifying the installation...");
    const GameCheck installed = CheckGameFiles({layout.app0_stage, layout.image_stage}, builds);
    if (installed.state != GameFileState::kValid || installed.build != &build) {
        return Failure(InstallStatus::kPrepareFailed, installed.detail);
    }
    if (hooks.progress && !hooks.progress(total, total)) {
        return Failure(InstallStatus::kCancelled);
    }
    phase("Finishing...");
    return Activate(layout, app0_guard, &image_guard);
}

// Puts the staged folders in place of the installed ones; without an image the installed image is removed, since it
// was built from another installation. A failed step puts the installed folders back.
InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard) {
    const fs::path &app0 = layout.app0, &image = layout.image;
    FsError error;
    // A backup left by a failed restore is the previous installation.
    for (const auto& [own, backup] : {std::pair{&app0, &layout.app0_backup}, std::pair{&image, &layout.image_backup}}) {
        if (!installfs::Exists(*own) && installfs::Exists(*backup) && !RenameWithRetry(*backup, *own, error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot restore", std::move(error));
        }
    }
    const bool had_app0 = installfs::Exists(app0), had_image = installfs::Exists(image);
    if (!installfs::RemoveAll(layout.app0_backup, error) || !installfs::RemoveAll(layout.image_backup, error)) {
        return FsFailure(InstallStatus::kWriteFailed, "cannot clear", std::move(error));
    }
    if (had_app0 && !RenameWithRetry(app0, layout.app0_backup, error)) {
        return FsFailure(InstallStatus::kWriteFailed, "cannot move aside", std::move(error));
    }
    if (!RenameWithRetry(layout.app0_stage, app0, error)) {
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
        return FsFailure(InstallStatus::kWriteFailed, "cannot activate", std::move(error));
    }
    const auto restore_app0 = [&] {
        RenameWithRetry(app0, layout.app0_stage);
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
    };
    if (had_image && !RenameWithRetry(image, layout.image_backup, error)) {
        restore_app0();
        return FsFailure(InstallStatus::kWriteFailed, "cannot move aside", std::move(error));
    }
    if (image_guard && !RenameWithRetry(layout.image_stage, image, error)) {
        if (had_image) {
            RenameWithRetry(layout.image_backup, image);
        }
        restore_app0();
        return FsFailure(InstallStatus::kWriteFailed, "cannot activate", std::move(error));
    }
    app0_guard.Release();
    if (image_guard) {
        image_guard->Release();
    }
    installfs::RemoveAll(layout.app0_backup, error);
    installfs::RemoveAll(layout.image_backup, error);
    return {};
}

// Lays the files of `from` into `to` as links (copies across volumes); the installed game is not changed.
InstallResult LinkTree(const fs::path& from, const fs::path& to) {
    std::vector<SourceFile> files;
    if (FsError error; !installfs::ListFiles(from, files, error)) {
        return FsFailure(InstallStatus::kReadFailed, "cannot list", std::move(error));
    }
    uint64_t total = 0;
    for (const SourceFile& file : files) {
        total += file.size;
    }
    const InstallHooks no_progress;
    CopyContext copy{&no_progress, 0, total, false};
    for (const SourceFile& file : files) {
        FsError error;
        if (!installfs::CreateDirectories((to / file.relative).parent_path(), error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot create", std::move(error));
        }
        if (TransferFile(from / file.relative, to / file.relative, copy, error) == Transfer::kFailed) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot copy", std::move(error));
        }
        copy.done += file.size;
    }
    return {};
}

// Lays the files of an unpacked update over the staged game. A staged file is replaced, never written through: it may
// be a link into the installed game.
InstallResult OverlayUpdate(const fs::path& update, const fs::path& staged) {
    std::vector<SourceFile> files;
    if (FsError error; !installfs::ListFiles(update, files, error)) {
        return FsFailure(InstallStatus::kReadFailed, "cannot list", std::move(error));
    }
    for (const SourceFile& file : files) {
        const fs::path from = update / file.relative, to = staged / file.relative;
        FsError error;
        if (!installfs::CreateDirectories(to.parent_path(), error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot create", std::move(error));
        }
        if (!installfs::Remove(to, error) || !installfs::Rename(from, to, error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot place", std::move(error));
        }
    }
    return {};
}

InstallStatus FromPackage(PackageResult::Code code) {
    switch (code) {
    case PackageResult::kOk: return InstallStatus::kInstalled;
    case PackageResult::kNotPackage: return InstallStatus::kNotPackage;
    case PackageResult::kUnsupported: return InstallStatus::kUnsupportedPackage;
    case PackageResult::kIncomplete: return InstallStatus::kIncomplete;
    case PackageResult::kReadFailed: return InstallStatus::kReadFailed;
    case PackageResult::kWriteFailed: return InstallStatus::kWriteFailed;
    case PackageResult::kCancelled: return InstallStatus::kCancelled;
    }
    return InstallStatus::kReadFailed;
}

InstallResult FromPackageResult(const PackageResult& package) {
    if (package.fs.failed()) {
        InstallResult result = FsFailure(FromPackage(package.code), "", package.fs);
        result.detail = package.detail;
        return result;
    }
    return Failure(FromPackage(package.code), package.detail);
}

// Without long paths the game's own file layer cannot open MAX_PATH or more, so such an installation is refused
// before anything is written; staging has no such limit.
std::optional<InstallResult> MeasurePaths(const Layout& layout, const std::vector<fs::path>& relatives,
                                          InstallPaths& paths) {
    paths.target = installfs::PathLength(layout.app0);
    paths.staging = installfs::PathLength(layout.app0_stage);
    paths.long_paths = installfs::ProcessLongPathsEnabled();
    size_t longest = 0;
    for (const fs::path& relative : relatives) {
        if (relative.native().size() > longest) {
            longest = relative.native().size();
            paths.longest_relative = Utf8(relative);
        }
    }
    paths.longest_final = longest ? paths.target + 1 + longest : 0;
    paths.longest_staging = longest ? paths.staging + 1 + longest : 0;
#ifdef _WIN32
    if (!paths.long_paths && paths.longest_final >= installfs::kMaxPath) {
        InstallResult result = Failure(InstallStatus::kPathTooLong,
                                       "the installed game's longest file would be " +
                                           std::to_string(paths.longest_final) + " characters, Windows allows " +
                                           std::to_string(installfs::kMaxPath - 1) + " without long paths: " +
                                           Utf8(layout.app0 / PathFromUtf8(paths.longest_relative)));
        result.paths = paths;
        return result;
    }
#endif
    return std::nullopt;
}

std::vector<fs::path> RelativesOf(const std::vector<PackageFile>& files) {
    std::vector<fs::path> relatives;
    relatives.reserve(files.size());
    for (const PackageFile& file : files) {
        relatives.push_back(PathFromUtf8(file.path).make_preferred());
    }
    return relatives;
}

std::vector<fs::path> RelativesOf(const std::vector<SourceFile>& files) {
    std::vector<fs::path> relatives;
    relatives.reserve(files.size());
    for (const SourceFile& file : files) {
        relatives.push_back(file.relative);
    }
    return relatives;
}

// A package whose build is already completely and correctly prepared needs no (re)installing. Which build is
// effectively active is decided separately, by precedence (LastPreparedBuild): installing an older, already-prepared
// build here never changes it. Nothing is returned when the build still has to be installed.
std::optional<InstallResult> ReuseInstalledBuild(const GameRoots& roots, std::span<const GameBuild> builds,
                                                 const GameBuild& build, bool replace_active) {
    if (replace_active) {
        return std::nullopt; // repairing the active build by reinstalling it is still allowed
    }
    const GamePaths target = BuildPaths(roots, build);
    const GameCheck prepared = CheckGameFiles(target, builds);
    if (prepared.state != GameFileState::kValid || prepared.build != &build) {
        return std::nullopt;
    }
    // Installing an installed build of another region switches to that region; its files are already in place.
    if (const GameBuild* active = LastPreparedBuild(roots, builds);
        active && std::string_view(active->title_id) != build.title_id) {
        std::error_code ec;
        fs::last_write_time(target.app0 / kPreparedMarker, fs::file_time_type::clock::now(), ec);
    }
    return Failure(InstallStatus::kUpToDate, build.label);
}

// Install update upgrades the active game: an update of another title is refused, whatever other region is installed.
std::optional<InstallResult> RefuseUpdateOfOtherTitle(const GameRoots& roots, const InstallHooks& hooks,
                                                      std::span<const GameBuild> builds, const GameBuild& update) {
    if (hooks.expected != PackageKind::kUpdate || !IsUpdateBuild(update)) {
        return std::nullopt;
    }
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (!active || std::string_view(active->title_id) == update.title_id) {
        return std::nullopt;
    }
    InstallResult result = Failure(InstallStatus::kUpdateForOtherTitle,
                                   std::string(update.label) + " is an update of " + update.title_id +
                                       ", the active game is " + active->label);
    result.update_title_id = update.title_id;
    result.installed_title_id = active->title_id;
    return result;
}

// A package that names a supported build but carries another executable is diagnosed in place, before anything is
// unpacked; nothing is returned when the installation should go on.
std::optional<InstallResult> RejectForeignExecutable(Package& package, const GameCheck& identity) {
    const std::unique_ptr<GameSource> source = OpenPackageSource(package);
    if (!ExecutableDiffers(*source, *identity.build)) {
        return std::nullopt;
    }
    GameCheck check;
    check.state = GameFileState::kDifferentVersion;
    check.detail = "eboot.bin is not the supported build";
    check.build = identity.build;
    InstallResult result = UnsupportedOrFailure(check, *source, package.sfo());
    if (result.status != InstallStatus::kUnsupportedBuild) {
        return std::nullopt;
    }
    return result;
}

// A package that finishes its build by itself: the base package of a build without updates.
InstallResult InstallBasePackage(const fs::path& source, Package& package, const PackageResult& loaded, const Layout& layout,
                                 const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build,
                                 InstallPaths& paths, Leftovers& leftovers) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (!loaded.ok()) {
        return FromPackageResult(loaded);
    }
    if (std::optional<InstallResult> refused = MeasurePaths(layout, RelativesOf(package.Files()), paths)) {
        return *refused;
    }
    const uint64_t total = package.total_bytes();
    if (InstallResult prepared = PrepareStaging(layout, total); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage, hooks, leftovers), image_guard(layout.image_stage, hooks, leftovers);

    phase("Unpacking the game files...");
    if (const PackageResult extracted = package.Extract(layout.app0_stage, hooks.progress); !extracted.ok()) {
        return FromPackageResult(extracted);
    }
    if (FsError error; !WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full", error)) {
        return MarkerFailure(std::move(error));
    }
    phase("Checking the game files...");
    const GameCheck unpacked = CheckGameTree(layout.app0_stage, builds);
    if (unpacked.state != GameFileState::kValid || unpacked.build != &build) {
        return UnsupportedOrFailure(unpacked, layout.app0_stage);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

// An update laid over the installed base build of its title, staged and checked like any other installation.
InstallResult InstallUpdatePackage(const fs::path& source, Package& package, const PackageResult& loaded, const Layout& layout,
                                   const GameRoots& roots, const InstallHooks& hooks, std::span<const GameBuild> builds,
                                   const GameBuild& build, InstallPaths& paths, Leftovers& leftovers) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const GameBuild* base_build = BaseBuildOf(build, builds);
    if (!base_build) {
        return Failure(InstallStatus::kWrongVersion, std::string("no base package for ") + build.label);
    }
    const GamePaths base = BuildPaths(roots, *base_build);
    if (const GameCheck base_check = CheckGameTree(base.app0, builds);
        base_check.state != GameFileState::kValid || base_check.build != base_build) {
        return Failure(InstallStatus::kUpdateNeedsBase,
                       std::string("the base ") + base_build->label + " is not installed: " + base_check.detail);
    }
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (!loaded.ok()) {
        return FromPackageResult(loaded);
    }
    std::vector<SourceFile> base_files;
    if (FsError error; !installfs::ListFiles(base.app0, base_files, error)) {
        return FsFailure(InstallStatus::kReadFailed, "cannot list", std::move(error));
    }
    std::vector<fs::path> relatives = RelativesOf(base_files);
    for (fs::path& relative : RelativesOf(package.Files())) {
        relatives.push_back(std::move(relative));
    }
    if (std::optional<InstallResult> refused = MeasurePaths(layout, relatives, paths)) {
        return *refused;
    }
    const uint64_t total = package.total_bytes();
    if (InstallResult prepared = PrepareStaging(layout, total); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage, hooks, leftovers), image_guard(layout.image_stage, hooks, leftovers);

    const fs::path update_dir = WithSuffix(layout.app0, ".install-update");
    if (FsError error; !installfs::RemoveAll(update_dir, error)) {
        return FsFailure(InstallStatus::kWriteFailed, "cannot clear", std::move(error));
    }
    StagingGuard update_guard(update_dir, hooks, leftovers);
    phase("Unpacking the update...");
    if (const PackageResult extracted = package.Extract(update_dir, hooks.progress); !extracted.ok()) {
        return FromPackageResult(extracted);
    }
    phase("Copying the game files...");
    InstallResult staged = StageUpdate(base.app0, update_dir, layout.app0_stage);
    if (FsError error; !installfs::RemoveAll(update_dir, error)) {
        leftovers.push_back(error.Describe());
    }
    update_guard.Release();
    if (!staged.ok()) {
        return staged;
    }
    if (FsError error; !WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full", error)) {
        return MarkerFailure(std::move(error));
    }
    phase("Checking the game files...");
    const GameCheck merged = CheckGameTree(layout.app0_stage, builds);
    if (merged.state != GameFileState::kValid || merged.build != &build) {
        return UnsupportedOrFailure(merged, layout.app0_stage);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

} // namespace

InstallResult StageUpdate(const fs::path& base_app0, const fs::path& update_dir, const fs::path& staged_app0) {
    if (InstallResult linked = LinkTree(base_app0, staged_app0); !linked.ok()) {
        return linked;
    }
    return OverlayUpdate(update_dir, staged_app0);
}

bool ActivateInstalledBuild(const GameRoots& roots, const GameBuild& build, std::span<const GameBuild> builds) {
    const GameCheck prepared = CheckGameFiles(BuildPaths(roots, build), builds);
    return prepared.state == GameFileState::kValid && prepared.build == &build;
}

namespace {

constexpr char kRemovingSuffix[] = ".removing";

bool IsLinkOrJunction(const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec))) {
        return true;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(installfs::Extended(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}

// The build's own two folders, directly in the roots and named after the build; nothing else is ever removed.
bool OwnedFolders(const GameRoots& roots, const GameBuild& build, const Layout& layout) {
    const auto owned = [&](const fs::path& dir, const fs::path& root) {
        return dir.parent_path() == Normal(root) && dir.filename() == PathFromUtf8(build.folder) &&
               !IsLinkOrJunction(dir) && !IsLinkOrJunction(root);
    };
    return owned(layout.app0, roots.prepared) && owned(layout.image, roots.image);
}

bool RemoveAside(const fs::path& aside, std::string& detail) {
    if (!installfs::Exists(aside)) {
        return true;
    }
    if (FsError error; !installfs::RemoveAll(aside, error)) {
        detail += (detail.empty() ? "" : "; ") + Utf8(aside) + " left for later: " + error.Describe();
        return false;
    }
    return true;
}

} // namespace

RemoveResult RemoveInstalledUpdate(const GameRoots& roots, const GameBuild& build, std::span<const GameBuild> builds) {
    RemoveResult result;
    const auto fail = [&](RemoveStatus status, std::string detail) {
        result.status = status;
        result.detail = std::move(detail);
        return result;
    };
    const GameBuild* base = BaseBuildOf(build, builds);
    if (!base) {
        return fail(RemoveStatus::kNotUpdate, std::string(build.label) + " is not an update");
    }
    const Layout layout(BuildPaths(roots, build));
    if (!OwnedFolders(roots, build, layout)) {
        return fail(RemoveStatus::kForeignFolder, Utf8(layout.app0) + " is not the update's own folder");
    }
    if (!installfs::Exists(layout.app0) && !installfs::Exists(layout.image)) {
        return fail(RemoveStatus::kNotInstalled, std::string(build.label) + " is not installed");
    }
    std::string sfo;
    std::ifstream sfo_file;
    if (FsError ignored; installfs::OpenForRead(layout.app0 / "sce_sys" / "param.sfo", sfo_file, ignored)) {
        sfo.assign(std::istreambuf_iterator<char>(sfo_file), std::istreambuf_iterator<char>());
    }
    sfo_file.close();
    if (installfs::Exists(layout.app0) && CheckSfoIdentity(sfo, builds).build != &build) {
        return fail(RemoveStatus::kForeignFolder, Utf8(layout.app0) + " does not hold " + build.label);
    }
    const fs::path app0_aside = WithSuffix(layout.app0, kRemovingSuffix);
    const fs::path image_aside = WithSuffix(layout.image, kRemovingSuffix);
    RemoveAside(app0_aside, result.detail);
    RemoveAside(image_aside, result.detail);
    if (installfs::Exists(app0_aside) || installfs::Exists(image_aside)) {
        return fail(RemoveStatus::kInUse, "an earlier removal is still in use: " + result.detail);
    }
    FsError error;
    const bool app0_moved = !installfs::Exists(layout.app0) || RenameWithRetry(layout.app0, app0_aside, error);
    if (!app0_moved) {
        return fail(RemoveStatus::kInUse, Utf8(layout.app0) + " is in use: " + error.Describe());
    }
    if (installfs::Exists(layout.image) && !RenameWithRetry(layout.image, image_aside, error)) {
        if (installfs::Exists(app0_aside)) {
            RenameWithRetry(app0_aside, layout.app0);
        }
        return fail(RemoveStatus::kInUse, Utf8(layout.image) + " is in use: " + error.Describe());
    }
    RemoveAside(app0_aside, result.detail);
    RemoveAside(image_aside, result.detail);
    // The effective active build is recomputed by precedence (LastPreparedBuild), not forced: removing an update
    // selects the base only when no other update is still installed.
    result.base_selected = LastPreparedBuild(roots, builds) == base;
    return result;
}

std::vector<const GameBuild*> InstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                                 std::span<const GameBuild> builds) {
    std::vector<const GameBuild*> updates;
    for (const GameBuild* build : PreparedBuilds(roots, builds)) {
        if (IsUpdateBuild(*build) && title_id == build->title_id) {
            updates.push_back(build);
        }
    }
    return updates;
}

RemoveAllResult RemoveInstalledUpdatesOf(const GameRoots& roots, std::string_view title_id,
                                         std::span<const GameBuild> builds) {
    RemoveAllResult result;
    for (const GameBuild& build : builds) {
        if (!IsUpdateBuild(build) || title_id != build.title_id) {
            continue;
        }
        const RemoveResult removed = RemoveInstalledUpdate(roots, build, builds);
        if (removed.status == RemoveStatus::kRemoved) {
            ++result.removed;
        } else if (removed.status != RemoveStatus::kNotInstalled) {
            ++result.failed;
        }
        if (!removed.detail.empty() && removed.status != RemoveStatus::kNotInstalled) {
            result.detail += (result.detail.empty() ? "" : "; ") + removed.detail;
        }
    }
    for (const GameBuild& build : builds) {
        if (IsUpdateBuild(build) || title_id != build.title_id || PreparationScope(BuildPaths(roots, build).app0) != "full") {
            continue;
        }
        if (result.removed > 0) {
            std::error_code ec;
            fs::last_write_time(BuildPaths(roots, build).app0 / kPreparedMarker, fs::file_time_type::clock::now(), ec);
        }
        const GameBuild* active = LastPreparedBuild(roots, builds);
        result.base_selected = active == &build;
    }
    return result;
}

bool FinishInterruptedRemovals(const GameRoots& roots, std::span<const GameBuild> builds) {
    bool clean = true;
    std::string detail;
    for (const GameBuild& build : builds) {
        if (!IsUpdateBuild(build)) {
            continue;
        }
        const Layout layout(BuildPaths(roots, build));
        if (!OwnedFolders(roots, build, layout)) {
            continue;
        }
        clean = RemoveAside(WithSuffix(layout.app0, kRemovingSuffix), detail) && clean;
        clean = RemoveAside(WithSuffix(layout.image, kRemovingSuffix), detail) && clean;
    }
    return clean;
}

bool RefusedPackageKind(InstallStatus status) {
    return status == InstallStatus::kExpectedBasePackage || status == InstallStatus::kExpectedUpdatePackage;
}

InstallResult InstallGame(const fs::path& source, const GameRoots& roots, const InstallHooks& hooks,
                          std::span<const GameBuild> builds) {
    return installfs::IsDirectory(source) ? InstallFromFolder(source, roots, hooks, builds)
                                          : InstallFromPackage(source, roots, hooks, builds);
}

// The package as recognized: its build, whether it is the base or an update, and the base an update goes over.
static std::string PackageRecognition(const fs::path& file, const GameCheck& identity, std::span<const GameBuild> builds) {
    const GameBuild& build = *identity.build;
    std::string text = "package recognized: " + Utf8(file.filename()) + " is " + build.label + " (" +
                       (identity.layer > 0 ? "update" : "base") + ", layer " + std::to_string(identity.layer + 1) + " of " +
                       std::to_string(build.layers.size());
    if (const GameBuild* base = BaseBuildOf(build, builds); base && identity.layer > 0) {
        text += std::string(", laid over ") + base->label;
    }
    return text + ")";
}

// A failure reports the step the player saw, not the cleanup after it.
template <class Body>
static InstallResult Tracked(const InstallHooks& hooks, Body body) {
    struct Track {
        std::string phase;
        uint64_t done = 0, total = 0;
    };
    const auto track = std::make_shared<Track>();
    InstallHooks tracked = hooks;
    tracked.phase = [track, outer = hooks.phase](const char* text) {
        if (std::string_view(text) != kCleanupPhase) {
            track->phase = text;
            track->done = track->total = 0;
        }
        if (outer) {
            outer(text);
        }
    };
    tracked.progress = [track, outer = hooks.progress](uint64_t done, uint64_t total) {
        track->done = done;
        track->total = total;
        return outer ? outer(done, total) : true;
    };
    InstallPaths paths;
    Leftovers leftovers;
    InstallResult result = body(tracked, paths, leftovers);
    if (result.paths.target == 0) {
        result.paths = paths;
    }
    result.leftovers = std::move(leftovers);
    if (!result.ok()) {
        result.stage = track->phase;
        result.stage_done = track->done;
        result.stage_total = track->total;
    }
    return result;
}

static InstallResult InstallFromPackageFile(const fs::path& package_file, const GameRoots& roots,
                                            const InstallHooks& hooks, std::span<const GameBuild> builds,
                                            InstallPaths& paths, Leftovers& leftovers) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const fs::path source = Normal(package_file);

    phase("Checking the package...");
    if (!installfs::IsRegularFile(source)) {
        return Failure(InstallStatus::kNotPackage, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    Package package;
    if (const PackageResult opened = package.Open(source); !opened.ok()) {
        return FromPackageResult(opened);
    }
    const GameCheck identity = CheckPackageIdentity(package.sfo(), builds);
    if (identity.state != GameFileState::kValid) {
        if (identity.state == GameFileState::kDifferentVersion && package.LoadFiles().ok()) {
            return UnsupportedOrFailure(identity, *OpenPackageSource(package), package.sfo());
        }
        return Failure(FromCheck(identity), identity.detail);
    }
    const GameBuild& build = *identity.build;
    const std::string recognition = PackageRecognition(source, identity, builds);
    const auto recognized = [&](InstallResult result) {
        result.recognition = recognition;
        return result;
    };
    if (hooks.expected == PackageKind::kBase && identity.layer > 0) {
        return recognized(Failure(InstallStatus::kExpectedBasePackage, std::string("update package for ") + build.label));
    }
    if (hooks.expected == PackageKind::kUpdate && identity.layer == 0) {
        return recognized(Failure(InstallStatus::kExpectedUpdatePackage, std::string("base package for ") + build.label));
    }
    if (std::optional<InstallResult> refused = RefuseUpdateOfOtherTitle(roots, hooks, builds, build)) {
        return recognized(*refused);
    }
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, hooks.replace_active)) {
        return recognized(*reused);
    }
    const PackageResult loaded = package.LoadFiles();
    if (loaded.ok()) {
        if (std::optional<InstallResult> foreign = RejectForeignExecutable(package, identity)) {
            return recognized(*foreign);
        }
    }
    const Layout layout(BuildPaths(roots, build));
    if (identity.layer > 0) {
        return recognized(
            InstallUpdatePackage(source, package, loaded, layout, roots, hooks, builds, build, paths, leftovers));
    }
    return recognized(InstallBasePackage(source, package, loaded, layout, hooks, builds, build, paths, leftovers));
}

InstallResult InstallFromPackage(const fs::path& package_file, const GameRoots& roots, const InstallHooks& hooks,
                                 std::span<const GameBuild> builds) {
    return Tracked(hooks, [&](const InstallHooks& tracked, InstallPaths& paths, Leftovers& leftovers) {
        return InstallFromPackageFile(package_file, roots, tracked, builds, paths, leftovers);
    });
}

static InstallResult InstallFromGameFolder(const fs::path& source_dir, const GameRoots& roots, const InstallHooks& hooks,
                                           std::span<const GameBuild> builds, InstallPaths& paths, Leftovers& leftovers) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const fs::path source = Normal(source_dir);

    phase("Checking the game folder...");
    if (!installfs::IsRegularFile(source / "eboot.bin") || !installfs::IsRegularFile(source / "sce_sys" / "param.sfo")) {
        return Failure(InstallStatus::kNotGameFolder, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    const GameCheck check = CheckGameTree(source, builds);
    if (check.state != GameFileState::kValid) {
        return UnsupportedOrFailure(check, source);
    }
    const GameBuild& build = *check.build;
    if (hooks.expected == PackageKind::kBase && IsUpdateBuild(build)) {
        return Failure(InstallStatus::kExpectedBasePackage, std::string("update folder for ") + build.label);
    }
    if (hooks.expected == PackageKind::kUpdate && !IsUpdateBuild(build)) {
        return Failure(InstallStatus::kExpectedUpdatePackage, std::string("base folder for ") + build.label);
    }
    if (std::optional<InstallResult> refused = RefuseUpdateOfOtherTitle(roots, hooks, builds, build)) {
        return *refused;
    }
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, hooks.replace_active)) {
        return *reused;
    }
    const Layout layout(BuildPaths(roots, build));
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }

    std::vector<SourceFile> files;
    if (FsError error; !installfs::ListFiles(source, files, error, IsSkipped)) {
        return FsFailure(InstallStatus::kReadFailed, "cannot list", std::move(error));
    }
    uint64_t total = 0;
    for (const SourceFile& file : files) {
        total += file.size;
    }
    if (std::optional<InstallResult> refused = MeasurePaths(layout, RelativesOf(files), paths)) {
        return *refused;
    }

    // The same drive gets hard links, which need no space.
    if (InstallResult prepared = PrepareStaging(layout, SameVolume(source, layout.app0) ? 0 : total);
        !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage, hooks, leftovers), image_guard(layout.image_stage, hooks, leftovers);

    phase("Copying the game files...");
    CopyContext copy{&hooks, 0, total, false};
    size_t linked = 0;
    for (const SourceFile& file : files) {
        const fs::path from = source / file.relative, to = layout.app0_stage / file.relative;
        FsError error;
        if (!installfs::CreateDirectories(to.parent_path(), error)) {
            return FsFailure(InstallStatus::kWriteFailed, "cannot create", std::move(error));
        }
        switch (TransferFile(from, to, copy, error)) {
        case Transfer::kLinked: ++linked; break;
        case Transfer::kCopied: break;
        case Transfer::kCancelled: return Failure(InstallStatus::kCancelled);
        case Transfer::kFailed: return FsFailure(InstallStatus::kWriteFailed, "cannot copy", std::move(error));
        }
        copy.done += file.size;
        if (hooks.progress && !hooks.progress(copy.done, total)) {
            return Failure(InstallStatus::kCancelled);
        }
    }
    if (FsError error; !WriteMarker(layout.app0_stage, source, linked, files.size() - linked, build, "full", error)) {
        return MarkerFailure(std::move(error));
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = files.size();
    result.linked = linked;
    result.build = &build;
    return result;
}

InstallResult InstallFromFolder(const fs::path& source_dir, const GameRoots& roots, const InstallHooks& hooks,
                                std::span<const GameBuild> builds) {
    return Tracked(hooks, [&](const InstallHooks& tracked, InstallPaths& paths, Leftovers& leftovers) {
        return InstallFromGameFolder(source_dir, roots, tracked, builds, paths, leftovers);
    });
}

const char* InstallStatusKey(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled: return "installed";
    case InstallStatus::kCancelled: return "cancelled";
    case InstallStatus::kNotPackage: return "not_package";
    case InstallStatus::kUnsupportedPackage: return "unsupported_package";
    case InstallStatus::kNotGameFolder: return "not_game_folder";
    case InstallStatus::kWrongGame: return "wrong_game";
    case InstallStatus::kWrongVersion: return "wrong_version";
    case InstallStatus::kIncomplete: return "incomplete";
    case InstallStatus::kReadFailed: return "read_failed";
    case InstallStatus::kOverlapping: return "overlapping";
    case InstallStatus::kForeignFolder: return "foreign_folder";
    case InstallStatus::kNoSpace: return "no_space";
    case InstallStatus::kWriteFailed: return "write_failed";
    case InstallStatus::kPrepareFailed: return "prepare_failed";
    case InstallStatus::kUpdateNeedsBase: return "update_needs_base";
    case InstallStatus::kUpToDate: return "up_to_date";
    case InstallStatus::kUnsupportedBuild: return "unsupported_build";
    case InstallStatus::kExpectedBasePackage: return "expected_base_package";
    case InstallStatus::kExpectedUpdatePackage: return "expected_update_package";
    case InstallStatus::kUpdateForOtherTitle: return "update_for_other_title";
    case InstallStatus::kPathTooLong: return "path_too_long";
    }
    return "unknown";
}

InstallMessage DescribeInstall(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled:
        return {Tr("Game installed"), Tr("The game files are in place.")};
    case InstallStatus::kCancelled:
        return {Tr("Installation cancelled"), Tr("The installed game was not changed.")};
    case InstallStatus::kNotPackage:
        return {Tr("Not a PS4 package"), Tr("Select the God of War: Sons of Sparta .pkg file.")};
    case InstallStatus::kUnsupportedPackage:
        return {Tr("This package cannot be unpacked"),
                Tr("It is encrypted for a PS4 console. Select a decrypted (fake) PKG of the game.")};
    case InstallStatus::kNotGameFolder:
        return {Tr("Not a game folder"),
                Tr("Select the folder that contains eboot.bin, sce_module and sce_sys.")};
    case InstallStatus::kWrongGame:
        return {Tr("Wrong game"), Tr("This is not God of War: Sons of Sparta.")};
    case InstallStatus::kWrongVersion:
        return {Tr("Different version of God of War: Sons of Sparta"),
                Tr("This port supports ") + SupportedBuildsText() + "."};
    case InstallStatus::kIncomplete:
        return {Tr("Game files missing"),
                Tr("The package is incomplete or some of the game's files are missing. Copy or download it again.")};
    case InstallStatus::kReadFailed:
        return {Tr("Could not read the game files"),
                Tr("The package could not be read or is damaged. Check the file, then try again.")};
    case InstallStatus::kOverlapping:
        return {Tr("Choose another file"),
                Tr("Select your game package, not a file inside the port's own game folder.")};
    case InstallStatus::kForeignFolder:
        return {Tr("Could not install the game"),
                Tr("The port's game folder holds other files. Move them out and try again.")};
    case InstallStatus::kNoSpace:
        return {Tr("Not enough free space"),
                Tr("The game needs up to 5 GB and an update up to 1.3 GB more. Free up disk space and try again.")};
    case InstallStatus::kWriteFailed:
        return {Tr("Could not install the game"), Tr("The game files could not be written next to the port.")};
    case InstallStatus::kPrepareFailed:
        return {Tr("Could not prepare the game"),
                Tr("The game's code could not be prepared. The log has the details.")};
    case InstallStatus::kUpdateNeedsBase:
        return {Tr("Base game required"),
                Tr("This package is an update. Install the base game package of the same title ID (CUSA number) "
                   "first, then install the update.")};
    case InstallStatus::kUpToDate:
        return {Tr("Already installed"), Tr("This version is already installed.")};
    case InstallStatus::kExpectedBasePackage:
        return {Tr("This is an update package"),
                Tr("Install game needs the base game. Use Install update for an update package.")};
    case InstallStatus::kExpectedUpdatePackage:
        return {Tr("This is the base game package"),
                Tr("Install update needs an update package. Use Install game for the base game.")};
    case InstallStatus::kUpdateForOtherTitle:
        return {Tr("Update for another game version"),
                Tr("Install the matching game version before applying this update.")};
    case InstallStatus::kUnsupportedBuild:
        return {Tr("God of War: Sons of Sparta version not supported yet"),
                Tr("Diagnostic information was written to the latest log. Please share the latest log when reporting "
                   "this version on GitHub.")};
    case InstallStatus::kPathTooLong:
        return {Tr("The port's folder path is too long"),
                Tr("Windows cannot open the game's longest files from this folder. Move the port to a shorter folder, "
                   "such as C:\\Games\\Sons of Sparta, or turn on Windows long paths, then try again.")};
    }
    return {Tr("Could not install the game"), Tr("An unexpected error occurred.")};
}

InstallMessage DescribeInstall(const InstallResult& result) {
    InstallMessage message = DescribeInstall(result.status);
    if (result.status == InstallStatus::kUpdateForOtherTitle) {
        message.title = Tr("This update is for ") + result.update_title_id + Tr(", but the installed game is ") +
                        result.installed_title_id;
    }
    if (result.status == InstallStatus::kWriteFailed) {
        switch (result.fs_error.problem()) {
        case FsProblem::kAccessDenied:
            message.text = Tr("Access to the port's game folder was denied. Antivirus software or the folder's "
                              "permissions may be blocking it. The log has the details.");
            break;
        case FsProblem::kInUse:
            message.text = Tr("A game file is in use by another program, such as antivirus software. Close it and "
                              "try again.");
            break;
        case FsProblem::kPathTooLong:
            message.text = DescribeInstall(InstallStatus::kPathTooLong).text;
            break;
        case FsProblem::kInvalidName:
            message.text = Tr("The drive of the port's folder does not accept one of the game's file names. Move the "
                              "port to an NTFS drive and try again.");
            break;
        default: break;
        }
    }
    return message;
}

} // namespace sos
