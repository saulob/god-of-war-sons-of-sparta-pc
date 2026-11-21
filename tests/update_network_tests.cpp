// Update network tests: the controller against a scripted loopback HTTP server. Every failure must end in a
// terminal state that offers a retry, and a retry must start clean.
// Run: sos-update-network-tests

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <trg/download.h>

#include "updater/sha256.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_controller.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"
#include "update_config.h"
#include "update_test_server.h"

using namespace trg::update;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

using namespace update_test;

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("sos-update-network-tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

std::string MakeZip(const std::string& name, const std::string& content) {
  mz_zip_archive archive{};
  if (mz_zip_writer_init_heap(&archive, 0, 0) == MZ_FALSE ||
      mz_zip_writer_add_mem(&archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION) == MZ_FALSE) {
    return {};
  }
  void* data = nullptr;
  size_t size = 0;
  std::string result;
  if (mz_zip_writer_finalize_heap_archive(&archive, &data, &size) != MZ_FALSE) {
    result.assign(static_cast<const char*>(data), size);
    mz_free(data);
  }
  mz_zip_writer_end(&archive);
  return result;
}

constexpr const char* kPackageName = "Package-v1.1.0.zip";
constexpr const char* kChecksumName = "Package-v1.1.0.zip.sha256";

struct Fixture {
  Server server;
  TempDir temp;
  std::string package = MakeZip("app.exe", "new launcher");

  std::string ReleaseJson(bool with_digest) {
    const std::string digest = with_digest ? ",\"digest\":\"sha256:" + Sha256Hex(package) + "\"" : std::string();
    const std::string checksum = "{\"name\":\"" + std::string(kChecksumName) + "\",\"browser_download_url\":\"" +
                                 server.Url("/checksum") + "\",\"size\":100}";
    return "{\"tag_name\":\"v1.1.0\",\"draft\":false,\"prerelease\":false,\"html_url\":\"" + server.Url("/page") +
           "\",\"assets\":[{\"name\":\"" + kPackageName + "\",\"browser_download_url\":\"" + server.Url("/package") +
           "\",\"size\":" + std::to_string(package.size()) + digest + "}," + checksum + "]}";
  }

  UpdateConfig Config() {
    UpdateConfig config;
    config.current_version = "1.0.0";
    config.releases_url = server.Url("/release");
    config.user_agent = "update-network-tests";
    config.asset_name = [](const SemanticVersion&) { return std::string(kPackageName); };
    config.checksum_asset_name = [](const SemanticVersion&) { return std::string(kChecksumName); };
    config.install_dir = temp.path / "install";
    config.restart_executable = config.install_dir / "app.exe";
    config.required_staged_files = {"app.exe"};
    config.install_supported = true;
    config.url_filter = [this](const std::string& url) { return server.Allows(url); };
    config.connect_timeout_ms = 2000;
    config.receive_timeout_ms = 400;
    config.retry.max_attempts = 1;
    return config;
  }

  void ServeRelease(bool with_digest) { server.Script("/release", {Body(ReleaseJson(with_digest))}); }
  void ServeChecksum() { server.Script("/checksum", {Body(Sha256Hex(package) + "  " + kPackageName + "\n")}); }
};

bool WaitUntil(const std::function<bool()>& condition, int limit_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

bool Working(UpdateState state) {
  return state == UpdateState::kChecking || state == UpdateState::kDownloading || state == UpdateState::kVerifying;
}

// Polls like the launcher does until the controller leaves its working states, or the deadline passes.
UpdateSnapshot Settle(UpdateController& controller, int limit_ms = 15000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
  do {
    controller.Poll();
    if (!Working(controller.snapshot().state) && !controller.Busy()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  return controller.snapshot();
}

UpdateSnapshot CheckAndSettle(UpdateController& controller) {
  controller.StartCheck(true);
  return Settle(controller);
}

UpdateSnapshot DownloadAndSettle(UpdateController& controller) {
  controller.StartDownload();
  return Settle(controller);
}

UpdateSnapshot RetryAndSettle(UpdateController& controller) {
  controller.Retry();
  return Settle(controller);
}

int Workspaces() {
  int count = 0;
  const std::string prefix = "trg-update-" + std::to_string(CurrentProcessId()) + "-";
  std::error_code ec;
  for (std::filesystem::directory_iterator it(std::filesystem::temp_directory_path(), ec), end; !ec && it != end;
       it.increment(ec)) {
    if (Utf8Path(it->path().filename()).rfind(prefix, 0) == 0) {
      ++count;
    }
  }
  return count;
}

void ExpectFailedWithRetry(const UpdateSnapshot& snapshot) {
  CHECK(snapshot.state == UpdateState::kFailed);
  CHECK(snapshot.retry_available);
  CHECK(!snapshot.error.empty());
}

void TestReleaseCheckFailures() {
  std::printf("release check failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());

  fixture.server.Script("/release", {Status(504), Body(fixture.ReleaseJson(true))});
  UpdateSnapshot snapshot = CheckAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.retry_check);
  CHECK(snapshot.error.find("504") != std::string::npos);
  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kAvailable);
  CHECK(snapshot.latest_version == "1.1.0");
  CHECK(fixture.server.Requests("/release") == 2);

  Fixture closed;
  UpdateController closed_controller(closed.Config());
  closed.server.Script("/release", {CutShort(closed.ReleaseJson(true)), Body(closed.ReleaseJson(true))});
  snapshot = CheckAndSettle(closed_controller);
  ExpectFailedWithRetry(snapshot);
  snapshot = RetryAndSettle(closed_controller);
  CHECK(snapshot.state == UpdateState::kAvailable);

  Fixture stalled;
  UpdateController stalled_controller(stalled.Config());
  stalled.server.Script("/release", {Stall(stalled.ReleaseJson(true)), Body(stalled.ReleaseJson(true))});
  const auto started = std::chrono::steady_clock::now();
  snapshot = CheckAndSettle(stalled_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
  snapshot = RetryAndSettle(stalled_controller);
  CHECK(snapshot.state == UpdateState::kAvailable);

  Fixture large;
  UpdateController large_controller(large.Config());
  large.server.Script("/release", {Body(std::string(size_t(5) << 20, ' '))});
  ExpectFailedWithRetry(CheckAndSettle(large_controller));
}

void TestChecksumFailures() {
  std::printf("checksum request failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(false);
  fixture.server.Script("/package", {Body(fixture.package)});
  fixture.server.Script("/checksum",
                        {Status(504), Body(Sha256Hex(fixture.package) + "  " + kPackageName + "\n")});
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  const int workspaces = Workspaces();
  UpdateSnapshot snapshot = DownloadAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(!snapshot.retry_check);
  CHECK(snapshot.error.find("504") != std::string::npos);
  CHECK(fixture.server.Requests("/package") == 0);
  CHECK(Workspaces() == workspaces);

  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);
  CHECK(fixture.server.Requests("/checksum") == 2);
}

void TestPackageFailures() {
  std::printf("package download failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  fixture.server.Script("/package", {Status(504), Body(fixture.package)});
  const int workspaces = Workspaces();
  UpdateSnapshot snapshot = DownloadAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("504") != std::string::npos);
  CHECK(snapshot.progress < 0.0f);
  CHECK(Workspaces() == workspaces);
  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);

  Fixture closed;
  UpdateController closed_controller(closed.Config());
  closed.ServeRelease(true);
  CHECK(CheckAndSettle(closed_controller).state == UpdateState::kAvailable);
  closed.server.Script("/package", {CutShort(closed.package), CutShort(closed.package), Body(closed.package)});
  const int closed_workspaces = Workspaces();
  snapshot = DownloadAndSettle(closed_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(Workspaces() == closed_workspaces);
  snapshot = RetryAndSettle(closed_controller);
  ExpectFailedWithRetry(snapshot);
  snapshot = RetryAndSettle(closed_controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);
  CHECK(closed.server.Requests("/package") == 3);

  Fixture stalled;
  UpdateController stalled_controller(stalled.Config());
  stalled.ServeRelease(true);
  CHECK(CheckAndSettle(stalled_controller).state == UpdateState::kAvailable);
  stalled.server.Script("/package", {Stall(stalled.package), Body(stalled.package)});
  const auto started = std::chrono::steady_clock::now();
  snapshot = DownloadAndSettle(stalled_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(!stalled_controller.Busy());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
  snapshot = RetryAndSettle(stalled_controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);

  Fixture oversized;
  UpdateController oversized_controller(oversized.Config());
  oversized.ServeRelease(true);
  CHECK(CheckAndSettle(oversized_controller).state == UpdateState::kAvailable);
  oversized.server.Script("/package", {Body(std::string(size_t(2) << 20, 'x'))});
  ExpectFailedWithRetry(DownloadAndSettle(oversized_controller));

  Fixture corrupt;
  UpdateController corrupt_controller(corrupt.Config());
  corrupt.ServeRelease(true);
  CHECK(CheckAndSettle(corrupt_controller).state == UpdateState::kAvailable);
  std::string altered = corrupt.package;
  altered.back() = static_cast<char>(altered.back() ^ 0x55);
  corrupt.server.Script("/package", {Body(altered)});
  snapshot = DownloadAndSettle(corrupt_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("SHA-256") != std::string::npos);
}

void TestRedirects() {
  std::printf("redirects\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  fixture.server.Script("/package", {Redirect(fixture.server.Url("/asset"))});
  fixture.server.Script("/asset", {Body(fixture.package)});
  CHECK(DownloadAndSettle(controller).state == UpdateState::kReadyToRestart);

  Fixture foreign;
  UpdateController foreign_controller(foreign.Config());
  foreign.ServeRelease(true);
  CHECK(CheckAndSettle(foreign_controller).state == UpdateState::kAvailable);
  foreign.server.Script("/package", {Redirect("http://localhost:1/package")});
  UpdateSnapshot snapshot = DownloadAndSettle(foreign_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("not allowed") != std::string::npos);

  Fixture loop;
  UpdateController loop_controller(loop.Config());
  loop.ServeRelease(true);
  CHECK(CheckAndSettle(loop_controller).state == UpdateState::kAvailable);
  loop.server.Script("/package", {Redirect(loop.server.Url("/package"))});
  snapshot = DownloadAndSettle(loop_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("redirected too many times") != std::string::npos);
  CHECK(loop.server.Requests("/package") == 6);

  Fixture malformed;
  UpdateController malformed_controller(malformed.Config());
  malformed.ServeRelease(true);
  CHECK(CheckAndSettle(malformed_controller).state == UpdateState::kAvailable);
  malformed.server.Script("/package", {Redirect("::not a url")});
  ExpectFailedWithRetry(DownloadAndSettle(malformed_controller));
}

void TestAddressPolicyInController() {
  std::printf("address policy in the controller\n");
  Fixture fixture;
  UpdateConfig config = fixture.Config();
  config.url_filter = nullptr;
  config.releases_url = "https://example.com/releases/latest";
  config.download_url_prefix = "https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/";
  UpdateController controller(config);
  const UpdateSnapshot snapshot = CheckAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.retry_check);

  UpdateConfig http_config = fixture.Config();
  http_config.url_filter = nullptr;
  UpdateController http_controller(http_config);
  fixture.ServeRelease(true);
  ExpectFailedWithRetry(CheckAndSettle(http_controller));
  CHECK(fixture.server.Requests("/release") == 0);
}

class LogLines {
 public:
  std::function<void(std::string_view)> Sink() {
    return [this](std::string_view text) {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.emplace_back(text);
    };
  }
  bool Contains(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(lines_.begin(), lines_.end(),
                       [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> lines_;
};

// GitHub's latest-release address answers 404 until the first release is published.
void TestNoReleasePublished() {
  std::printf("no release published yet\n");
  for (const bool manual : {false, true}) {
    Fixture fixture;
    fixture.server.Script("/release", {Status(404)});
    LogLines log;
    UpdateConfig config = fixture.Config();
    config.log = log.Sink();
    UpdateController controller(config);
    controller.StartCheck(manual);
    UpdateSnapshot snapshot = Settle(controller);
    CHECK(snapshot.state == UpdateState::kNoUpdate);
    CHECK(snapshot.no_release);
    CHECK(snapshot.error.empty());
    CHECK(!snapshot.retry_available);
    CHECK(fixture.server.Requests("/release") == 1);
    CHECK(log.Contains("No release has been published yet"));
    CHECK(!log.Contains("failed"));
    CHECK(!log.Contains("404"));
    controller.StartCheck(true);
    snapshot = Settle(controller);
    CHECK(snapshot.state == UpdateState::kNoUpdate);
    CHECK(fixture.server.Requests("/release") == 1);
  }

  Fixture published;
  published.server.Script("/release", {Status(404), Body(published.ReleaseJson(true))});
  UpdateController before(published.Config());
  CHECK(CheckAndSettle(before).state == UpdateState::kNoUpdate);
  UpdateController after(published.Config());
  const UpdateSnapshot found = CheckAndSettle(after);
  CHECK(found.state == UpdateState::kAvailable);
  CHECK(!found.no_release);
  CHECK(found.latest_version == "1.1.0");

  for (const int status : {403, 500}) {
    Fixture limited;
    limited.server.Script("/release", {Status(status)});
    LogLines log;
    UpdateConfig config = limited.Config();
    config.log = log.Sink();
    UpdateController quiet(config);
    quiet.StartCheck(false);
    const UpdateSnapshot automatic = Settle(quiet);
    CHECK(automatic.state == UpdateState::kIdle);
    CHECK(!automatic.no_release);
    CHECK(log.Contains("Update check failed"));
    CHECK(log.Contains(std::to_string(status)));
    UpdateController asked(limited.Config());
    const UpdateSnapshot requested = CheckAndSettle(asked);
    ExpectFailedWithRetry(requested);
    CHECK(!requested.no_release);
  }

  Fixture missing;
  missing.ServeRelease(true);
  missing.server.Script("/package", {Status(404)});
  UpdateController package_controller(missing.Config());
  CHECK(CheckAndSettle(package_controller).state == UpdateState::kAvailable);
  const UpdateSnapshot download = DownloadAndSettle(package_controller);
  ExpectFailedWithRetry(download);
  CHECK(download.error.find("404") != std::string::npos);
}

void TestProductionConfig() {
  std::printf("production update address and asset names\n");
  TempDir temp;
  const UpdateConfig config = sos::MakeUpdateConfig(temp.path);
  CHECK(config.releases_url == "https://api.github.com/repos/saulob/god-of-war-sons-of-sparta-pc/releases/latest");
  CHECK(config.download_url_prefix == "https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/");
  CHECK(config.current_version == SOS_PORT_VERSION);
  CHECK(config.user_agent == std::string("SonsOfSpartaPC-Updater/") + SOS_PORT_VERSION);
  CHECK(!config.url_filter);
  CHECK(UrlHostAllowed(config.releases_url, config.allowed_hosts));
  const std::optional<SemanticVersion> current = ParseVersion(SOS_PORT_VERSION);
  CHECK(current && current->stable());

  const SemanticVersion next = *ParseVersion("0.3.0");
#ifdef _WIN32
  CHECK(config.package_kind == UpdatePackageKind::kZip);
  CHECK(config.asset_name(next) == "SonsOfSpartaPC-v0.3.0-windows-x64.zip");
  CHECK(config.checksum_asset_name(next) == "SonsOfSpartaPC-v0.3.0-windows-x64.zip.sha256");
  CHECK(config.restart_executable.filename() == "sons_of_sparta.exe");
#else
  CHECK(config.package_kind == UpdatePackageKind::kAppImage);
  CHECK(config.asset_name(next) == "SonsOfSpartaPC-v0.3.0-x86_64.AppImage");
  CHECK(config.checksum_asset_name(next) == "SonsOfSpartaPC-v0.3.0-x86_64.AppImage.sha256");
#endif
  const std::string asset = "https://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v0.3.0/" + config.asset_name(next);
  CHECK(UrlAllowed(asset, config.allowed_hosts, config.download_url_prefix));
  CHECK(UrlHostAllowed("https://release-assets.githubusercontent.com/github-production-release-asset/1/2",
                       config.allowed_hosts));
  CHECK(!UrlAllowed("https://github.com/saulob/other-project/releases/download/v0.3.0/" + config.asset_name(next),
                    config.allowed_hosts, config.download_url_prefix));
  CHECK(!UrlAllowed("http://github.com/saulob/god-of-war-sons-of-sparta-pc/releases/download/v0.3.0/" + config.asset_name(next),
                    config.allowed_hosts, config.download_url_prefix));
  CHECK(!UrlHostAllowed("https://github.com.example.com/saulob/god-of-war-sons-of-sparta-pc", config.allowed_hosts));
  CHECK(!UrlHostAllowed("https://user@github.com/saulob/god-of-war-sons-of-sparta-pc", config.allowed_hosts));
}

#ifndef _WIN32
std::string ReadAll(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return {};
  }
  std::string text;
  char buffer[512];
  for (size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
    text.append(buffer, count);
  }
  std::fclose(file);
  return text;
}

void WriteExecutable(const std::filesystem::path& path, const std::string& text) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file) {
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
  chmod(path.c_str(), 0755);
}

bool WaitForFile(const std::filesystem::path& path) {
  for (int i = 0; i < 400 && ReadAll(path).find('\n') == std::string::npos; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ReadAll(path).find('\n') != std::string::npos;
}

// The AppImage updater replaces the AppImage file the player started ($APPIMAGE), never a file inside its read-only
// mount, and starts that same file again. The running copy keeps reading the old file until it exits.
void TestAppImageReplacementAndRelaunch() {
  std::printf("AppImage replacement and relaunch\n");
  TempDir temp;
  const std::filesystem::path games = temp.path / "Games";
  const std::filesystem::path downloads = temp.path / "download";
  std::filesystem::create_directories(games);
  std::filesystem::create_directories(downloads);
  const std::filesystem::path installed = games / "SonsOfSpartaPC-v0.1.0-x86_64.AppImage";
  const std::filesystem::path marker = games / "started.txt";
  WriteExecutable(installed, "#!/bin/sh\necho old\n");

  const char* previous = std::getenv("APPIMAGE");
  const std::string saved = previous ? previous : "";
  const char* previous_dir = std::getenv("APPDIR");
  const std::string saved_dir = previous_dir ? previous_dir : "";
  setenv("APPIMAGE", installed.c_str(), 1);
  unsetenv("APPDIR");
  std::filesystem::path found;
  CHECK(!HasOriginalAppImage(found));
  setenv("APPDIR", (temp.path / "elsewhere").c_str(), 1);
  CHECK(!HasOriginalAppImage(found));
  setenv("APPDIR", CurrentExecutablePath().parent_path().c_str(), 1);
  CHECK(HasOriginalAppImage(found));
  CHECK(found == std::filesystem::weakly_canonical(installed));
  const UpdateConfig config = sos::MakeUpdateConfig(temp.path / "mount" / "usr" / "bin");
  CHECK(config.install_supported);
  CHECK(config.appimage_path == found);
  CHECK(config.restart_executable == found);
  CHECK(!config.restart_arguments.empty() && config.restart_arguments.back() == "--launcher");
  CHECK(std::none_of(config.restart_arguments.begin(), config.restart_arguments.end(),
                     [](const std::string& a) { return a.rfind("--sos-skip-launcher", 0) == 0; }));

  const int running = open(installed.c_str(), O_RDONLY);
  CHECK(running >= 0);
  const std::filesystem::path update = downloads / "SonsOfSpartaPC-v0.3.0-x86_64.AppImage";
  WriteExecutable(update, "#!/bin/sh\nprintf '%s %s\\n' \"$0\" \"$1\" > \"$(dirname \"$0\")/started.txt\"\n");
  std::string error;
  CHECK(ApplyAppImageUpdate(update, found, {"--launcher"}, error));
  CHECK(error.empty());
  CHECK(WaitForFile(marker));
  CHECK(ReadAll(marker) == Utf8Path(found) + " --launcher\n");
  CHECK(ReadAll(installed) == ReadAll(update));
  char old[64] = {};
  CHECK(running >= 0 && pread(running, old, sizeof(old) - 1, 0) > 0 && std::string(old).find("echo old") != std::string::npos);
  if (running >= 0) {
    close(running);
  }
  int leftovers = 0;
  for (const auto& entry : std::filesystem::directory_iterator(games)) {
    leftovers += Utf8Path(entry.path().filename()).find(".trg-update-") != std::string::npos;
  }
  CHECK(leftovers == 0);

  // A replacement that cannot start puts the previous AppImage back.
  const std::string before = ReadAll(installed);
  const std::filesystem::path broken = downloads / "broken.AppImage";
  WriteExecutable(broken, "not a program");
  CHECK(!ApplyAppImageUpdate(broken, found, {"--launcher"}, error));
  CHECK(!error.empty());
  CHECK(ReadAll(installed) == before);
  CHECK(!ApplyAppImageUpdate(found, found, {}, error));

  if (previous) {
    setenv("APPIMAGE", saved.c_str(), 1);
  } else {
    unsetenv("APPIMAGE");
  }
  if (previous_dir) {
    setenv("APPDIR", saved_dir.c_str(), 1);
  } else {
    unsetenv("APPDIR");
  }
  CHECK(!HasOriginalAppImage(found) || previous);
}
#endif

std::map<std::string, std::string> Tree(const std::filesystem::path& root) {
  std::map<std::string, std::string> files;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      std::ifstream file(it->path(), std::ios::binary);
      std::ostringstream text;
      text << file.rdbuf();
      files[Utf8Path(it->path().lexically_relative(root))] = text.str();
    }
  }
  return files;
}

void WriteInstall(const std::filesystem::path& install) {
  std::filesystem::create_directories(install / "assets");
  for (const auto& [name, text] : std::map<std::string, std::string>{
           {"app.exe", "OLD LAUNCHER"}, {"trg-updater.exe", "OLD HELPER"}, {"assets/a.png", "OLD ASSET"}}) {
    std::ofstream(install / name, std::ios::binary) << text;
  }
}

UpdateConfig RetryingConfig(Fixture& fixture) {
  UpdateConfig config = fixture.Config();
  config.retry.max_attempts = 3;
  config.retry.base_delay_ms = 20;
  config.retry.max_delay_ms = 100;
  return config;
}

void TestAutomaticRetries() {
  std::printf("automatic retries inside one check or download\n");
  Fixture fixture;
  UpdateController controller(RetryingConfig(fixture));
  fixture.server.Script("/release", {Status(503), Body(fixture.ReleaseJson(true))});
  controller.StartCheck(true);
  bool failed_between = false;
  UpdateSnapshot snapshot;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  do {
    controller.Poll();
    snapshot = controller.snapshot();
    failed_between = failed_between || snapshot.state == UpdateState::kFailed;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (Working(snapshot.state) && std::chrono::steady_clock::now() < deadline);
  CHECK(snapshot.state == UpdateState::kAvailable && !failed_between);
  CHECK(snapshot.attempts == 2 && snapshot.failure == UpdateFailure::kNone);
  CHECK(fixture.server.Requests("/release") == 2);

  fixture.server.Script("/package", {CutShort(fixture.package), Status(502), Body(fixture.package)});
  snapshot = DownloadAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart && snapshot.attempts == 3);
  CHECK(fixture.server.Requests("/package") == 3);

  Fixture down;
  UpdateController down_controller(RetryingConfig(down));
  down.ServeRelease(true);
  CHECK(CheckAndSettle(down_controller).state == UpdateState::kAvailable);
  down.server.Script("/package", {Status(503)});
  const int workspaces = Workspaces();
  snapshot = DownloadAndSettle(down_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kHttpStatus && snapshot.attempts == 3);
  CHECK(snapshot.error.find("503") != std::string::npos);
  CHECK(down.server.Requests("/package") == 3);
  CHECK(Workspaces() == workspaces);
  down.server.Script("/package", {Body(down.package)});
  CHECK(RetryAndSettle(down_controller).state == UpdateState::kReadyToRestart);

  Fixture permanent;
  UpdateController permanent_controller(RetryingConfig(permanent));
  permanent.ServeRelease(true);
  CHECK(CheckAndSettle(permanent_controller).state == UpdateState::kAvailable);
  permanent.server.Script("/package", {Status(404), Body(permanent.package)});
  snapshot = DownloadAndSettle(permanent_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.attempts == 1 && permanent.server.Requests("/package") == 1);
  CHECK(snapshot.failure == UpdateFailure::kHttpStatus);

  Fixture checksum;
  UpdateController checksum_controller(RetryingConfig(checksum));
  checksum.ServeRelease(false);
  checksum.server.Script("/package", {Body(checksum.package)});
  checksum.server.Script("/checksum", {Status(500), Body(Sha256Hex(checksum.package) + "  " + kPackageName + "\n")});
  CHECK(CheckAndSettle(checksum_controller).state == UpdateState::kAvailable);
  snapshot = DownloadAndSettle(checksum_controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart && snapshot.attempts == 1);
  CHECK(checksum.server.Requests("/checksum") == 2);
}

void TestFailureCategories() {
  std::printf("failure categories reach the snapshot\n");
  Fixture oversized;
  UpdateController oversized_controller(oversized.Config());
  oversized.ServeRelease(true);
  CHECK(CheckAndSettle(oversized_controller).state == UpdateState::kAvailable);
  oversized.server.Script("/package", {Body(oversized.package + "extra")});
  UpdateSnapshot snapshot = DownloadAndSettle(oversized_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kSizeLimit);

  Fixture corrupt;
  UpdateController corrupt_controller(corrupt.Config());
  corrupt.ServeRelease(true);
  CHECK(CheckAndSettle(corrupt_controller).state == UpdateState::kAvailable);
  std::string altered = corrupt.package;
  altered.back() = static_cast<char>(altered.back() ^ 0x55);
  corrupt.server.Script("/package", {Body(altered)});
  snapshot = DownloadAndSettle(corrupt_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kChecksum);

  Fixture short_body;
  UpdateController short_controller(short_body.Config());
  short_body.ServeRelease(true);
  CHECK(CheckAndSettle(short_controller).state == UpdateState::kAvailable);
  short_body.server.Script("/package", {Body(short_body.package.substr(0, short_body.package.size() - 1))});
  snapshot = DownloadAndSettle(short_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kContentLength || snapshot.failure == UpdateFailure::kInterrupted);

  Fixture stalled;
  UpdateController stalled_controller(stalled.Config());
  stalled.ServeRelease(true);
  CHECK(CheckAndSettle(stalled_controller).state == UpdateState::kAvailable);
  stalled.server.Script("/package", {Stall(stalled.package)});
  snapshot = DownloadAndSettle(stalled_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kReceiveTimeout);

  Fixture loop;
  UpdateController loop_controller(loop.Config());
  loop.ServeRelease(true);
  CHECK(CheckAndSettle(loop_controller).state == UpdateState::kAvailable);
  loop.server.Script("/package", {Redirect(loop.server.Url("/package"))});
  snapshot = DownloadAndSettle(loop_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kRedirectLimit);

  Fixture limited;
  UpdateController limited_controller(limited.Config());
  limited.server.Script("/release", {Status(403)});
  snapshot = CheckAndSettle(limited_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.failure == UpdateFailure::kHttpStatus && snapshot.retry_check);
}

void TestCancelledUpdate() {
  std::printf("cancelling a check and a download\n");
  Fixture fixture;
  UpdateConfig config = RetryingConfig(fixture);
  config.receive_timeout_ms = 10000;
  UpdateController controller(config);
  fixture.server.Script("/release", {Hang()});
  controller.StartCheck(true);
  CHECK(WaitUntil([&] { return fixture.server.Requests("/release") >= 1; }, 3000));
  controller.Cancel();
  UpdateSnapshot snapshot = Settle(controller);
  CHECK(snapshot.state == UpdateState::kIdle && snapshot.failure == UpdateFailure::kCancelled);
  CHECK(snapshot.error.empty() && !snapshot.retry_available);
  CHECK(fixture.server.Requests("/release") == 1);

  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);
  fixture.server.Script("/package", {Hang()});
  const int workspaces = Workspaces();
  controller.StartDownload();
  CHECK(WaitUntil([&] { return fixture.server.Requests("/package") >= 1; }, 3000));
  controller.Cancel();
  snapshot = Settle(controller);
  CHECK(snapshot.state == UpdateState::kAvailable && snapshot.failure == UpdateFailure::kCancelled);
  CHECK(snapshot.error.empty() && !snapshot.retry_available && snapshot.progress < 0.0f);
  CHECK(fixture.server.Requests("/package") == 1);
  CHECK(Workspaces() == workspaces && !controller.Busy());

  fixture.server.Script("/package", {Body(fixture.package)});
  snapshot = DownloadAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart && snapshot.failure == UpdateFailure::kNone);

  Fixture retrying;
  UpdateConfig slow = RetryingConfig(retrying);
  slow.retry.base_delay_ms = 5000;
  slow.retry.max_delay_ms = 5000;
  UpdateController retrying_controller(slow);
  retrying.ServeRelease(true);
  CHECK(CheckAndSettle(retrying_controller).state == UpdateState::kAvailable);
  retrying.server.Script("/package", {Status(503), Body(retrying.package)});
  retrying_controller.StartDownload();
  CHECK(WaitUntil([&] { return retrying.server.Requests("/package") >= 1; }, 3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto cancelled_at = std::chrono::steady_clock::now();
  retrying_controller.Cancel();
  snapshot = Settle(retrying_controller);
  CHECK(snapshot.failure == UpdateFailure::kCancelled && snapshot.state == UpdateState::kAvailable);
  CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(3));
  CHECK(retrying.server.Requests("/package") == 1);
}

void TestInstalledPackageUntouched() {
  std::printf("a failed download leaves the installed package alone\n");
  Fixture fixture;
  WriteInstall(fixture.temp.path / "install");
  const std::map<std::string, std::string> before = Tree(fixture.temp.path / "install");
  CHECK(before.size() == 3);
  UpdateController controller(RetryingConfig(fixture));
  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  std::string altered = fixture.package;
  altered.back() = static_cast<char>(altered.back() ^ 0x55);
  const std::vector<std::pair<const char*, std::vector<Step>>> scripts = {
      {"missing", {Status(404)}},
      {"unavailable", {Status(503)}},
      {"cut", {CutShort(fixture.package)}},
      {"corrupt", {Body(altered)}},
      {"oversized", {Body(fixture.package + "extra")}},
      {"stalled", {Stall(fixture.package)}},
  };
  for (const auto& [name, steps] : scripts) {
    fixture.server.Script("/package", steps);
    const UpdateSnapshot snapshot = DownloadAndSettle(controller);
    CHECK(snapshot.state == UpdateState::kFailed);
    CHECK(Tree(fixture.temp.path / "install") == before);
    (void)name;
  }
  fixture.server.Script("/package", {Body(fixture.package)});
  CHECK(RetryAndSettle(controller).state == UpdateState::kReadyToRestart);
  CHECK(Tree(fixture.temp.path / "install") == before);
}

std::string AssetJson(Server& server, const std::string& name, const std::string& path = "/package") {
  return "{\"name\":\"" + name + "\",\"browser_download_url\":\"" + server.Url(path) + "\",\"size\":1234}";
}

std::string ReleaseWith(Server& server, const std::string& tag, bool draft, bool prerelease,
                        const std::vector<std::string>& assets) {
  std::string list;
  for (const std::string& name : assets) {
    list += (list.empty() ? "" : ",") + AssetJson(server, name);
  }
  return "{\"tag_name\":\"" + tag + "\",\"draft\":" + (draft ? "true" : "false") +
         ",\"prerelease\":" + (prerelease ? "true" : "false") + ",\"html_url\":\"" + server.Url("/page") +
         "\",\"assets\":[" + list + "]}";
}

std::string ReplaceFirst(std::string text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  if (at != std::string::npos) {
    text.replace(at, from.size(), to);
  }
  return text;
}

void TestSelfUpdateSelection() {
  std::printf("self-update decisions with the production asset names\n");
  TempDir temp;
  Server server;
  UpdateConfig config = sos::MakeUpdateConfig(temp.path);
  config.current_version = "1.2.3";
  config.releases_url = server.Url("/release");
  config.url_filter = [&server](const std::string& url) { return server.Allows(url); };
  config.install_supported = true;
  config.log = nullptr;
  config.retry.max_attempts = 1;
  const auto name_for = [&](const char* version) { return config.asset_name(*ParseVersion(version)); };
  const auto checksum_for = [&](const char* version) { return config.checksum_asset_name(*ParseVersion(version)); };
  const std::string good = name_for("1.3.0");

  const auto check = [&](const std::string& release) {
    server.Script("/release", {Body(release)});
    UpdateController controller(config);
    controller.StartCheck(true);
    return Settle(controller);
  };
  const auto available = [&](const UpdateSnapshot& snapshot) { return snapshot.state == UpdateState::kAvailable; };

  UpdateSnapshot snapshot = check(ReleaseWith(server, "v1.3.0", false, false, {good, checksum_for("1.3.0")}));
  CHECK(available(snapshot) && snapshot.latest_version == "1.3.0" && snapshot.asset_name == good);
  CHECK(snapshot.current_version == "1.2.3");
  snapshot = check(ReleaseWith(server, "1.3.0", false, false, {good}));
  CHECK(available(snapshot));
  snapshot = check(ReleaseWith(server, "v1.2.4", false, false, {name_for("1.2.4")}));
  CHECK(available(snapshot));
  snapshot = check(ReleaseWith(server, "v1.10.0", false, false, {name_for("1.10.0")}));
  CHECK(available(snapshot) && snapshot.latest_version == "1.10.0");
  snapshot = check(ReleaseWith(server, "v2.0.0", false, false, {name_for("2.0.0")}));
  CHECK(available(snapshot));

  for (const char* version : {"1.2.3", "1.2.2", "1.1.9", "1.0.0", "0.9.9", "0.0.1"}) {
    snapshot = check(ReleaseWith(server, std::string("v") + version, false, false, {name_for(version)}));
    CHECK(snapshot.state == UpdateState::kNoUpdate && snapshot.asset_name.empty());
  }
  snapshot = check(ReleaseWith(server, "v1.9.0", false, false, {name_for("1.9.0")}));
  CHECK(available(snapshot));
  snapshot = check(ReleaseWith(server, "v1.2.3", false, false, {good}));
  CHECK(snapshot.state == UpdateState::kNoUpdate);

  for (const char* tag : {"vbanana", "v", "", "v1", "v1.2", "v1.x.0", "v-1.0.0", "v1.2.3.4.5"}) {
    snapshot = check(ReleaseWith(server, tag, false, false, {good}));
    CHECK(!available(snapshot));
  }

  snapshot = check(ReleaseWith(server, "v1.3.0", true, false, {good}));
  CHECK(snapshot.state == UpdateState::kNoUpdate);
  snapshot = check(ReleaseWith(server, "v1.3.0", false, true, {good}));
  CHECK(snapshot.state == UpdateState::kNoUpdate);
  snapshot = check(ReleaseWith(server, "v1.4.0-beta.1", false, false, {name_for("1.4.0")}));
  CHECK(snapshot.state == UpdateState::kNoUpdate);
  snapshot = check(ReleaseWith(server, "v1.4.0-rc1", false, false, {good}));
  CHECK(snapshot.state == UpdateState::kNoUpdate);

  const std::vector<std::string> wrong = {
      good + ".sig",
      "Other" + good,
      ReplaceFirst(good, "SonsOfSpartaPC", "SonsOfSparta"),
      ReplaceFirst(ReplaceFirst(good, "x64", "arm64"), "x86_64", "aarch64"),
      ReplaceFirst(ReplaceFirst(good, "windows", "linux"), "zip", "tar.gz"),
      ReplaceFirst(good, ".zip", "-debug.zip"),
      ReplaceFirst(good, "v1.3.0", "v1.3.1"),
      name_for("1.2.3"),
      checksum_for("1.3.0"),
      " " + good,
      good + " ",
  };
  for (const std::string& name : wrong) {
    snapshot = check(ReleaseWith(server, "v1.3.0", false, false, {name}));
    CHECK(!available(snapshot) && snapshot.state == UpdateState::kFailed);
  }
  std::string lower = good;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  snapshot = check(ReleaseWith(server, "v1.3.0", false, false, {lower}));
  CHECK(lower == good || !available(snapshot));

  std::vector<std::string> crowd = wrong;
  crowd.push_back(good);
  snapshot = check(ReleaseWith(server, "v1.3.0", false, false, crowd));
  CHECK(available(snapshot) && snapshot.asset_name == good);

  std::string foreign_host = ReleaseWith(server, "v1.3.0", false, false, {good});
  foreign_host = ReplaceFirst(foreign_host, server.Url("/package"), "http://203.0.113.9/package");
  snapshot = check(foreign_host);
  CHECK(snapshot.state == UpdateState::kFailed);
}

void TestTransportTimeouts() {
  std::printf("transport timeouts\n");
  Fixture fixture;
  trg::HttpOptions options;
  options.receive_timeout_ms = 300;
  options.total_timeout_ms = 3000;
  options.allow_url = [&](const std::string& url) { return fixture.server.Allows(url); };

  std::string out;
  fixture.server.Script("/slow", {Stall("0123456789")});
  const auto started = std::chrono::steady_clock::now();
  CHECK(!trg::HttpGet(fixture.server.Url("/slow"), out, nullptr, options).empty());
  CHECK(out.empty());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));

  fixture.server.Script("/ok", {Body("hello")});
  CHECK(trg::HttpGet(fixture.server.Url("/ok"), out, nullptr, options).empty());
  CHECK(out == "hello");
  CHECK(!trg::HttpGet("http://127.0.0.1:1/ok", out, nullptr, options).empty());
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestReleaseCheckFailures();
  TestChecksumFailures();
  TestPackageFailures();
  TestRedirects();
  TestAddressPolicyInController();
  TestNoReleasePublished();
  TestProductionConfig();
#ifndef _WIN32
  TestAppImageReplacementAndRelaunch();
#endif
  TestAutomaticRetries();
  TestFailureCategories();
  TestCancelledUpdate();
  TestInstalledPackageUntouched();
  TestSelfUpdateSelection();
  TestTransportTimeouts();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
