// Updater HTTP hardening tests on the production transport (third_party/trg-launcher download.cpp) and the retry layer
// (launcher/updater/update_http.cpp), against a scripted loopback server: timeouts, size limits, Content-Length,
// interrupted transfers, the .part file, redirects, credential headers, transient statuses, retries, the result and
// the log. No Internet. A section name as the first argument runs only that section.
// Run: sos-update-http-tests [section]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <trg/download.h>
#include <trg/platform.h>

#include "update_test_server.h"
#include "updater/update_http.h"

using namespace trg::update;
using namespace update_test;

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

using Clock = std::chrono::steady_clock;

long long Since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

bool WaitUntil(const std::function<bool()>& condition, int limit_ms) {
  const auto end = Clock::now() + std::chrono::milliseconds(limit_ms);
  while (Clock::now() < end) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

class TempDir {
 public:
  TempDir() {
    const auto now = Clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("sos-update-http-tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string Utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

trg::HttpOptions Options(int receive_ms = 2000, int total_ms = 15000) {
  trg::HttpOptions options;
  options.connect_timeout_ms = 1000;
  options.receive_timeout_ms = receive_ms;
  options.total_timeout_ms = total_ms;
  options.user_agent = "update-http-tests";
  return options;
}

struct Probe {
  trg::HttpReport report;
  std::string out;
  std::string error;
  long long elapsed_ms = 0;
};

Probe Get(const std::string& url, trg::HttpOptions options) {
  Probe probe;
  options.report = &probe.report;
  const auto started = Clock::now();
  probe.error = trg::HttpGet(url, probe.out, nullptr, options);
  probe.elapsed_ms = Since(started);
  return probe;
}

RetryPolicy Policy(int attempts, int base_ms = 20, int max_ms = 200) {
  RetryPolicy policy;
  policy.max_attempts = attempts;
  policy.base_delay_ms = base_ms;
  policy.max_delay_ms = max_ms;
  return policy;
}

TransferResult Fetch(const std::string& url, std::string& out, const trg::HttpOptions& options, const RetryPolicy& policy,
                     trg::Task* task = nullptr) {
  return FetchText("test", url, out, task, options, policy);
}

// Runs a transfer on a Task so the test can cancel it from outside.
class Running {
 public:
  explicit Running(std::function<void(trg::Task&)> work) {
    task.Start("test", [work = std::move(work)](trg::Task& self) {
      work(self);
      return std::string();
    });
  }
  ~Running() {
    task.Cancel();
    Wait();
  }
  void Wait() {
    WaitUntil([this] { return !task.running(); }, 20000);
    task.Finish();
  }
  trg::Task task;
};

#ifdef _WIN32
// A listener whose backlog is full: further connections are not answered, so a connect has to time out.
class FullBacklog {
 public:
  FullBacklog() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener_, 1);
    int length = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    for (int i = 0; i < 32; ++i) {
      SOCKET client = socket(AF_INET, SOCK_STREAM, 0);
      u_long nonblocking = 1;
      ioctlsocket(client, FIONBIO, &nonblocking);
      connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      clients_.push_back(client);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  ~FullBacklog() {
    for (const SOCKET client : clients_) {
      closesocket(client);
    }
    closesocket(listener_);
    WSACleanup();
  }
  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

 private:
  SOCKET listener_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::vector<SOCKET> clients_;
};
#endif

bool Runs(int argc, char** argv, const char* section) {
  return argc < 2 || std::strcmp(argv[1], section) == 0;
}

void TestTimeouts() {
  std::printf("timeouts\n");
  Server server;

  server.Script("/hang", {Hang()});
  Probe probe = Get(server.Url("/hang"), Options(250));
  CHECK(!probe.error.empty());
  CHECK(probe.report.failure == trg::HttpFailure::kReceiveTimeout);
  CHECK(probe.report.timed_out && probe.report.transient && !probe.report.cancelled);
  CHECK(probe.report.status == 0);
  CHECK(probe.elapsed_ms < 2500);

  server.Script("/stall", {Stall("0123456789")});
  probe = Get(server.Url("/stall"), Options(250));
  CHECK(probe.report.failure == trg::HttpFailure::kReceiveTimeout);
  CHECK(probe.report.status == 200 && probe.report.bytes == 5 && probe.report.expected == 10);
  CHECK(probe.out.empty());

  server.Script("/drip", {Drip(1000000)});
  probe = Get(server.Url("/drip"), Options(400, 700));
  CHECK(probe.report.failure == trg::HttpFailure::kTotalTimeout);
  CHECK(probe.report.timed_out && !probe.report.transient);
  CHECK(probe.report.bytes > 0);
  CHECK(probe.elapsed_ms < 2500);

  server.Script("/ok", {Body("hello")});
  probe = Get(server.Url("/ok"), Options(250, 2000));
  CHECK(probe.error.empty() && probe.out == "hello");
  CHECK(probe.report.failure == trg::HttpFailure::kNone && !probe.report.timed_out);

  server.Script("/r0", {Delayed(Redirect(server.Url("/r1")), 250)});
  server.Script("/r1", {Delayed(Redirect(server.Url("/r2")), 250)});
  server.Script("/r2", {Delayed(Redirect(server.Url("/r3")), 250)});
  server.Script("/r3", {Delayed(Body("late"), 250)});
  probe = Get(server.Url("/r0"), Options(1000, 700));
  CHECK(probe.report.failure == trg::HttpFailure::kTotalTimeout);
  CHECK(probe.report.redirects >= 1 && probe.report.redirects < 4);
  CHECK(probe.elapsed_ms < 2500);
  probe = Get(server.Url("/r0"), Options(1000, 5000));
  CHECK(probe.error.empty() && probe.out == "late" && probe.report.redirects == 3);

#ifdef _WIN32
  {
    FullBacklog backlog;
    trg::HttpOptions options = Options(4000, 8000);
    options.connect_timeout_ms = 300;
    probe = Get(backlog.Url(), options);
    CHECK(probe.report.failure == trg::HttpFailure::kConnectTimeout);
    CHECK(probe.report.timed_out && probe.report.transient);
    CHECK(probe.elapsed_ms < 3000);
  }
#endif

  server.Script("/busy", {Status(503)});
  std::string out;
  TransferResult result = Fetch(server.Url("/busy"), out, Options(300, 900), Policy(10, 200, 1000));
  CHECK(!result.ok);
  CHECK(result.attempts >= 2 && result.attempts < 10);
  CHECK(result.deadline_limited);
  CHECK(result.failure == UpdateFailure::kHttpStatus);
  CHECK(result.elapsed_ms < 1500);
  CHECK(server.Requests("/busy") == result.attempts);

  server.Script("/hang2", {Hang()});
  result = Fetch(server.Url("/hang2"), out, Options(200, 700), Policy(10, 10, 50));
  CHECK(!result.ok && result.failure == UpdateFailure::kTotalTimeout);
  CHECK(result.timed_out() && !result.cancelled());
  CHECK(result.elapsed_ms < 2500);
}

void TestCancellation() {
  std::printf("cancellation\n");
  Server server;
  std::string out;
  TransferResult result;

  server.Script("/hang", {Hang()});
  {
    Running run([&](trg::Task& task) {
      result = Fetch(server.Url("/hang"), out, Options(5000, 20000), Policy(3, 10, 50), &task);
    });
    CHECK(WaitUntil([&] { return server.Requests("/hang") >= 1; }, 3000));
    const auto cancelled_at = Clock::now();
    run.task.Cancel();
    run.Wait();
    CHECK(Since(cancelled_at) < 2000);
  }
  CHECK(!result.ok && result.cancelled() && !result.timed_out());
  CHECK(result.attempts == 1);
  CHECK(server.Requests("/hang") == 1);

  server.Script("/busy", {Status(503)});
  result = TransferResult();
  {
    Running run([&](trg::Task& task) {
      result = Fetch(server.Url("/busy"), out, Options(300, 20000), Policy(3, 5000, 5000), &task);
    });
    CHECK(WaitUntil([&] { return server.Requests("/busy") >= 1; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    run.task.Cancel();
    run.Wait();
  }
  CHECK(result.cancelled() && result.attempts == 1);
  CHECK(result.elapsed_ms < 2500);
  CHECK(server.Requests("/busy") == 1);

  trg::Task cancelled_before;
  cancelled_before.Cancel();
  result = Fetch(server.Url("/busy"), out, Options(), Policy(3), &cancelled_before);
  CHECK(result.cancelled() && result.attempts == 0);
  CHECK(server.Requests("/busy") == 1);
}

void TestSizeLimits() {
  std::printf("size limits\n");
  Server server;
  trg::HttpOptions options = Options();

  const std::string hundred(100, 'a');
  server.Script("/hundred", {Body(hundred)});
  options.max_bytes = 100;
  Probe probe = Get(server.Url("/hundred"), options);
  CHECK(probe.error.empty() && probe.out == hundred);
  options.max_bytes = 99;
  probe = Get(server.Url("/hundred"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit && !probe.report.transient);
  CHECK(probe.report.bytes == 0 && probe.out.empty());

  server.Script("/declared", {Declaring(Body("x"), 1000)});
  options.max_bytes = 500;
  probe = Get(server.Url("/declared"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit);
  CHECK(probe.report.bytes == 0 && probe.report.expected == 1000);

  const std::string three_hundred(300, 'b');
  server.Script("/nolength", {WithoutLength(Body(three_hundred))});
  server.Script("/chunked", {Chunked(Body(three_hundred))});
  options.max_bytes = 200;
  for (const char* path : {"/nolength", "/chunked"}) {
    probe = Get(server.Url(path), options);
    CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit && !probe.report.transient);
    CHECK(probe.report.expected == -1 && probe.out.empty());
    CHECK(probe.report.bytes <= 200);
  }
  options.max_bytes = 300;
  for (const char* path : {"/nolength", "/chunked"}) {
    probe = Get(server.Url(path), options);
    CHECK(probe.error.empty() && probe.out == three_hundred);
  }

  options.max_bytes = 1000;
  for (const char* text : {"9223372036854775807", "9223372036854775808", "99999999999999999999", "-1", "+5", "abc", "1e3"}) {
    server.Script("/bad", {LengthText(Body("x"), text)});
    probe = Get(server.Url("/bad"), options);
    CHECK(!probe.error.empty() && probe.out.empty());
    CHECK(probe.report.bytes == 0);
    CHECK(probe.report.failure == trg::HttpFailure::kContentLength || probe.report.failure == trg::HttpFailure::kNetwork);
  }
  server.Script("/near", {LengthText(Body("x"), "999999999999999")});
  probe = Get(server.Url("/near"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit);

  struct Limit {
    const char* name;
    long long bytes;
  };
  for (const Limit limit : {Limit{"metadata", kMaxReleaseJsonBytes}, Limit{"checksum", kMaxChecksumBytes},
                            Limit{"package", kMaxPackageBytes}}) {
    server.Script("/limit", {Declaring(Body("x"), limit.bytes + 1)});
    options.max_bytes = limit.bytes;
    probe = Get(server.Url("/limit"), options);
    CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit);
    CHECK(probe.report.bytes == 0);
  }
  CHECK(kMaxChecksumBytes < kMaxReleaseJsonBytes && kMaxReleaseJsonBytes < kMaxPackageBytes);
  server.Script("/checksum-at-limit", {Body(std::string(size_t(kMaxChecksumBytes), 'c'))});
  options.max_bytes = kMaxChecksumBytes;
  probe = Get(server.Url("/checksum-at-limit"), options);
  CHECK(probe.error.empty() && probe.out.size() == size_t(kMaxChecksumBytes));
  server.Script("/metadata-over", {Body(std::string(size_t(kMaxChecksumBytes) + 1, 'c'))});
  probe = Get(server.Url("/metadata-over"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit);
}

void TestContentLength() {
  std::printf("Content-Length\n");
  Server server;
  trg::HttpOptions options = Options();

  server.Script("/exact", {Body("0123456789")});
  Probe probe = Get(server.Url("/exact"), options);
  CHECK(probe.error.empty() && probe.out == "0123456789");
  CHECK(probe.report.bytes == 10 && probe.report.expected == 10);

  server.Script("/cut", {CutShort("0123456789")});
  probe = Get(server.Url("/cut"), options);
  CHECK(!probe.error.empty() && probe.out.empty());
  CHECK(probe.report.transient && probe.report.interrupted);
  CHECK(probe.report.failure == trg::HttpFailure::kContentLength || probe.report.failure == trg::HttpFailure::kInterrupted);
  CHECK(probe.report.bytes < probe.report.expected);

  server.Script("/larger", {Declaring(Body("abc"), 10)});
  probe = Get(server.Url("/larger"), options);
  CHECK(!probe.error.empty() && probe.out.empty() && probe.report.transient);
  CHECK(probe.report.expected == 10 && probe.report.bytes == 3);

  server.Script("/smaller", {Declaring(Body("0123456789"), 4)});
  probe = Get(server.Url("/smaller"), options);
  CHECK((probe.error.empty() && probe.out == "0123" && probe.report.bytes == 4) || (!probe.error.empty() && probe.out.empty()));
  options.max_bytes = 3;
  probe = Get(server.Url("/smaller"), options);
  CHECK(!probe.error.empty() && probe.out.empty());
  CHECK(probe.report.failure == trg::HttpFailure::kSizeLimit || probe.report.failure == trg::HttpFailure::kNetwork);
}

void TestInterruptedFile() {
  std::printf("interrupted transfers and the .part file\n");
  Server server;
  TempDir temp;
  const std::filesystem::path destination = temp.path / "package.zip";
  const std::filesystem::path part = temp.path / "package.zip.part";
  const std::string body = "NEW-PACKAGE-CONTENT-0123456789";
  const auto download = [&](const char* path, const RetryPolicy& policy, trg::HttpOptions options = Options(250)) {
    trg::Task task;
    return FetchFile("package", server.Url(path), Utf8(destination), task, options, policy);
  };
  const auto untouched = [&] { return ReadFile(destination) == "OLD" && !std::filesystem::exists(part); };

  server.Script("/ok", {Body(body)});
  WriteFile(destination, "OLD");
  TransferResult result = download("/ok", Policy(1));
  CHECK(result.ok && result.attempts == 1 && result.bytes == int(body.size()));
  CHECK(ReadFile(destination) == body && !std::filesystem::exists(part));

  struct Failure {
    const char* path;
    Step step;
    UpdateFailure expected;
  };
  const std::vector<Failure> failures = {
      {"/missing", Status(404), UpdateFailure::kHttpStatus},
      {"/unavailable", Status(503), UpdateFailure::kHttpStatus},
      {"/cut", CutShort(body), UpdateFailure::kContentLength},
      {"/stall", Stall(body), UpdateFailure::kReceiveTimeout},
      {"/large", Body(std::string(2000, 'z')), UpdateFailure::kSizeLimit},
      {"/loop", Redirect(server.Url("/loop")), UpdateFailure::kRedirectLimit},
  };
  for (const Failure& failure : failures) {
    server.Script(failure.path, {failure.step});
    WriteFile(destination, "OLD");
    trg::HttpOptions options = Options(250);
    options.max_bytes = 1000;
    result = download(failure.path, Policy(1), options);
    CHECK(!result.ok);
    if (failure.expected == UpdateFailure::kContentLength) {
      CHECK(result.failure == UpdateFailure::kContentLength || result.failure == UpdateFailure::kInterrupted);
    } else {
      CHECK(result.failure == failure.expected);
    }
    CHECK(untouched());
  }

  WriteFile(destination, "OLD");
  trg::Task cancelled;
  cancelled.Cancel();
  result = FetchFile("package", server.Url("/ok"), Utf8(destination), cancelled, Options(), Policy(3));
  CHECK(result.cancelled() && untouched());

  WriteFile(part, std::string(500, 'S'));
  WriteFile(destination, "OLD");
  result = download("/ok", Policy(1));
  CHECK(result.ok && ReadFile(destination) == body && !std::filesystem::exists(part));

  server.Script("/flaky", {CutShort(body), Body(body)});
  WriteFile(destination, "OLD");
  const int before = server.Requests("/flaky");
  result = download("/flaky", Policy(3));
  CHECK(result.ok && result.attempts == 2 && server.Requests("/flaky") - before == 2);
  CHECK(ReadFile(destination) == body && !std::filesystem::exists(part));

  server.Script("/flaky-always", {CutShort(body)});
  WriteFile(destination, "OLD");
  result = download("/flaky-always", Policy(3));
  CHECK(!result.ok && result.attempts == 3 && untouched());

  const std::filesystem::path folder = temp.path / "folder-destination";
  std::filesystem::create_directories(folder);
  WriteFile(folder / "keep.txt", "keep");
  trg::Task task;
  result = FetchFile("package", server.Url("/ok"), Utf8(folder), task, Options(), Policy(1));
  CHECK(!result.ok && result.failure == UpdateFailure::kFilesystem);
  CHECK(ReadFile(folder / "keep.txt") == "keep" && !std::filesystem::exists(Utf8(folder) + ".part"));

  server.Script("/..%2f..%2fescape.txt", {Body(body)});
  std::filesystem::remove(destination);
  result = download("/..%2f..%2fescape.txt", Policy(1));
  std::set<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
    names.insert(Utf8(entry.path().filename()));
  }
  CHECK(result.ok && names == std::set<std::string>({"package.zip", "folder-destination"}));

  server.Script("/slow-file", {Drip(1000000)});
  WriteFile(destination, "OLD");
  bool part_seen = false;
  bool destination_old_while_running = true;
  {
    Running run([&](trg::Task& running) {
      result = FetchFile("package", server.Url("/slow-file"), Utf8(destination), running, Options(1000, 20000),
                         Policy(1));
    });
    part_seen = WaitUntil([&] { return std::filesystem::exists(part); }, 5000);
    destination_old_while_running = ReadFile(destination) == "OLD";
    run.task.Cancel();
    run.Wait();
  }
  CHECK(part_seen && destination_old_while_running);
  CHECK(result.cancelled() && untouched());
}

void TestRedirects() {
  std::printf("redirects\n");
  Server server;
  Server other;
  trg::HttpOptions options = Options();

  server.Script("/a", {Redirect(server.Url("/b"))});
  server.Script("/b", {Body("done")});
  Probe probe = Get(server.Url("/a"), options);
  CHECK(probe.error.empty() && probe.out == "done" && probe.report.redirects == 1);

  for (const int status : {301, 302, 303, 307, 308}) {
    server.Script("/s", {Redirect(server.Url("/b"), status)});
    probe = Get(server.Url("/s"), options);
    CHECK(probe.error.empty() && probe.out == "done");
  }
  server.Script("/multiple", {Redirect(server.Url("/b"), 300)});
  probe = Get(server.Url("/multiple"), options);
  CHECK(!probe.error.empty() && probe.report.failure == trg::HttpFailure::kHttpStatus);

  server.Script("/dir/a", {Redirect("/dir/b")});
  server.Script("/dir/b", {Body("absolute path")});
  server.Script("/dir/c", {Redirect("b")});
  server.Script("/dir/sub/d", {Redirect("../b")});
  server.Script("/dir/e", {Redirect("./b")});
  server.Script("/dir/q", {Redirect("b?x=1")});
  server.Script("/dir/b?x=1", {Body("absolute path")});
  for (const char* path : {"/dir/a", "/dir/c", "/dir/sub/d", "/dir/e", "/dir/q"}) {
    probe = Get(server.Url(path), options);
    CHECK(probe.error.empty() && probe.out == "absolute path");
  }

  for (int hops = 1; hops <= 5; ++hops) {
    for (int i = 0; i < hops; ++i) {
      server.Script("/chain" + std::to_string(i), {Redirect(server.Url("/chain" + std::to_string(i + 1)))});
    }
    server.Script("/chain" + std::to_string(hops), {Body("end")});
    probe = Get(server.Url("/chain0"), options);
    CHECK(probe.error.empty() && probe.report.redirects == hops);
  }
  for (int i = 0; i < 7; ++i) {
    server.Script("/long" + std::to_string(i), {Redirect(server.Url("/long" + std::to_string(i + 1)))});
  }
  probe = Get(server.Url("/long0"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kRedirectLimit && !probe.report.transient);
  CHECK(probe.report.redirects == 5);
  options.max_redirects = 2;
  probe = Get(server.Url("/long0"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kRedirectLimit && probe.report.redirects == 2);
  options.max_redirects = 5;

  server.Script("/loop", {Redirect(server.Url("/loop"))});
  probe = Get(server.Url("/loop"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kRedirectLimit);
  CHECK(server.Requests("/loop") == 6);

  server.Script("/nolocation", {Redirect("")});
  probe = Get(server.Url("/nolocation"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kInvalidRedirect && !probe.report.transient);
  for (const char* bad : {"::not a url", "http://user@127.0.0.1/x", "http://", "ftp://example.com/x"}) {
    server.Script("/malformed", {Redirect(bad)});
    probe = Get(server.Url("/malformed"), options);
    CHECK(probe.report.failure == trg::HttpFailure::kInvalidRedirect);
  }

  server.Script("/to-other", {Redirect(other.Url("/landing"))});
  other.Script("/landing", {Body("other")});
  options.allow_url = [&](const std::string& url) { return server.Allows(url); };
  probe = Get(server.Url("/to-other"), options);
  CHECK(probe.report.failure == trg::HttpFailure::kAddressNotAllowed);
  CHECK(other.Requests("/landing") == 0);
  options.allow_url = nullptr;
  probe = Get(server.Url("/to-other"), options);
  CHECK(probe.error.empty() && probe.out == "other");

  std::string target;
  CHECK(trg::ResolveRedirect("https://example.com/a/b", "/c", target) == trg::HttpFailure::kNone);
  CHECK(target == "https://example.com/c");
  CHECK(trg::ResolveRedirect("https://example.com/a/b", "c", target) == trg::HttpFailure::kNone);
  CHECK(target == "https://example.com/a/c");
  CHECK(trg::ResolveRedirect("https://example.com/a/b/c", "../../x", target) == trg::HttpFailure::kNone);
  CHECK(target == "https://example.com/x");
  CHECK(trg::ResolveRedirect("https://example.com/a", "../../../x", target) == trg::HttpFailure::kNone);
  CHECK(target == "https://example.com/x");
  CHECK(trg::ResolveRedirect("https://example.com/a", "//cdn.example.com/y", target) == trg::HttpFailure::kNone);
  CHECK(target == "https://cdn.example.com/y");
  CHECK(trg::ResolveRedirect("https://example.com/a", "https://example.net:8443/z?s=1", target) ==
        trg::HttpFailure::kNone);
  CHECK(target == "https://example.net:8443/z?s=1");
  CHECK(trg::ResolveRedirect("http://example.com/a", "https://example.com/b", target) == trg::HttpFailure::kNone);
  CHECK(trg::ResolveRedirect("http://example.com/a", "http://example.com/b", target) == trg::HttpFailure::kNone);
  CHECK(trg::ResolveRedirect("https://example.com/a", "http://example.com/b", target) == trg::HttpFailure::kHttpsDowngrade);
  CHECK(target.empty());
  CHECK(trg::ResolveRedirect("https://example.com/a", "HTTP://example.com/b", target) == trg::HttpFailure::kHttpsDowngrade);
  CHECK(trg::ResolveRedirect("https://example.com/a", "http://127.0.0.1:80/b", target) == trg::HttpFailure::kHttpsDowngrade);
  CHECK(trg::ResolveRedirect("https://example.com/a", "", target) == trg::HttpFailure::kInvalidRedirect);
  CHECK(trg::ResolveRedirect("https://example.com/a", "https://u:p@example.com/", target) ==
        trg::HttpFailure::kInvalidRedirect);
}

void TestOrigins() {
  std::printf("origins and credential headers\n");
  CHECK(trg::SameOrigin("https://example.com/a", "https://example.com/b?q=1"));
  CHECK(trg::SameOrigin("HTTPS://EXAMPLE.com:443/a", "https://example.com/b"));
  CHECK(trg::SameOrigin("http://example.com:80/", "http://Example.COM/"));
  CHECK(!trg::SameOrigin("https://example.com/", "https://example.com:8443/"));
  CHECK(!trg::SameOrigin("https://example.com/", "http://example.com/"));
  CHECK(!trg::SameOrigin("https://example.com/", "https://example.org/"));
  CHECK(!trg::SameOrigin("https://example.com/", "https://example.com.evil.org/"));
  CHECK(!trg::SameOrigin("https://example.com/", "https://sub.example.com/"));
  CHECK(!trg::SameOrigin("https://github.com/", "https://objects.githubusercontent.com/"));
  CHECK(!trg::SameOrigin("not a url", "not a url"));

  for (const char* name : {"Authorization", "authorization", "Proxy-Authorization", "Cookie", "Set-Cookie", "X-Auth-Token",
                           "X-GitHub-Token", "X-Api-Key", "X-Secret", "X-Amz-Security-Token"}) {
    CHECK(trg::IsSensitiveHeader(name));
  }
  for (const char* name : {"Accept", "User-Agent", "X-GitHub-Api-Version", "Cache-Control", "Range", "If-None-Match",
                           "Accept-Encoding", "Accept-Language"}) {
    CHECK(!trg::IsSensitiveHeader(name));
  }

  Server first;
  Server second;
  trg::HttpOptions options = Options();
  options.headers = {{"Authorization", "Bearer SECRET-TOKEN"},
                     {"Cookie", "session=SECRET-COOKIE"},
                     {"X-Custom-Token", "SECRET-CUSTOM"},
                     {"Accept", "application/octet-stream"},
                     {"X-GitHub-Api-Version", "2022-11-28"}};
  const auto has = [](const std::string& request, const char* text) { return request.find(text) != std::string::npos; };

  first.Script("/start", {Redirect(second.Url("/end"))});
  second.Script("/end", {Body("far")});
  Probe probe = Get(first.Url("/start"), options);
  CHECK(probe.error.empty() && probe.out == "far");
  const std::string kept = first.Request("/start", 0);
  const std::string moved = second.Request("/end", 0);
  CHECK(has(kept, "Authorization: Bearer SECRET-TOKEN") && has(kept, "Cookie: session=SECRET-COOKIE") &&
        has(kept, "X-Custom-Token: SECRET-CUSTOM"));
  CHECK(!has(moved, "SECRET") && !has(moved, "Authorization") && !has(moved, "Cookie") && !has(moved, "X-Custom-Token"));
  CHECK(has(moved, "Accept: application/octet-stream") && has(moved, "X-GitHub-Api-Version: 2022-11-28"));
  CHECK(has(moved, "User-Agent: update-http-tests"));

  first.Script("/same", {Redirect(first.Url("/same-end"))});
  first.Script("/same-end", {Body("near")});
  probe = Get(first.Url("/same"), options);
  CHECK(probe.error.empty());
  CHECK(has(first.Request("/same-end", 0), "Authorization: Bearer SECRET-TOKEN"));
  CHECK(has(first.Request("/same-end", 0), "Cookie: session=SECRET-COOKIE"));

  first.Script("/there", {Redirect(second.Url("/back"))});
  second.Script("/back", {Redirect(first.Url("/home"))});
  first.Script("/home", {Body("home")});
  probe = Get(first.Url("/there"), options);
  CHECK(probe.error.empty() && probe.out == "home");
  CHECK(!has(second.Request("/back", 0), "SECRET"));
  CHECK(has(first.Request("/home", 0), "Authorization: Bearer SECRET-TOKEN"));
}

void TestTransientStatuses() {
  std::printf("transient statuses and retries\n");
  Server server;
  std::string out;

  for (const int status : {408, 429, 500, 502, 503, 504}) {
    server.Script("/t", {Status(status)});
    Probe probe = Get(server.Url("/t"), Options());
    CHECK(probe.report.transient && probe.report.failure == trg::HttpFailure::kHttpStatus);
    CHECK(probe.report.status == status && ShouldRetry(probe.report));

    server.Script("/t", {Status(status), Body("fine")});
    TransferResult result = Fetch(server.Url("/t"), out, Options(), Policy(3));
    CHECK(result.ok && result.attempts == 2 && out == "fine");
    CHECK(server.Requests("/t") == 2);
  }
  for (const int status : {400, 401, 403, 404, 409, 410, 422}) {
    server.Script("/p", {Status(status), Body("fine")});
    TransferResult result = Fetch(server.Url("/p"), out, Options(), Policy(3));
    CHECK(!result.ok && result.attempts == 1 && result.status == status);
    CHECK(result.failure == UpdateFailure::kHttpStatus);
    CHECK(server.Requests("/p") == 1);
    server.Script("/p", {Status(status), Body("fine")});
    const Probe probe = Get(server.Url("/p"), Options());
    CHECK(!probe.report.transient && probe.report.status == status && !ShouldRetry(probe.report));
  }

  server.Script("/multi", {Status(503), Status(502), Body("third")});
  TransferResult result = Fetch(server.Url("/multi"), out, Options(), Policy(3));
  CHECK(result.ok && result.attempts == 3 && out == "third" && server.Requests("/multi") == 3);
  result = Fetch(server.Url("/multi"), out, Options(), Policy(2));
  CHECK(result.ok && result.attempts == 1 && server.Requests("/multi") == 4);

  server.Script("/down", {Status(503)});
  result = Fetch(server.Url("/down"), out, Options(), Policy(3));
  CHECK(!result.ok && result.attempts == 3 && server.Requests("/down") == 3);
  CHECK(result.status == 503 && result.failure == UpdateFailure::kHttpStatus);
  result = Fetch(server.Url("/down"), out, Options(), Policy(1));
  CHECK(!result.ok && result.attempts == 1 && server.Requests("/down") == 4);
  result = Fetch(server.Url("/down"), out, Options(), Policy(0));
  CHECK(!result.ok && result.attempts == 1);

  server.Script("/limited", {WithHeader(Status(429), "Retry-After: 1"), Body("later")});
  const auto started = Clock::now();
  result = Fetch(server.Url("/limited"), out, Options(), Policy(2, 10, 3000));
  CHECK(result.ok && result.attempts == 2 && Since(started) >= 900 && Since(started) < 3000);

  server.Script("/forever", {WithHeader(Status(503), "Retry-After: 99999"), Body("soon")});
  const auto bounded = Clock::now();
  result = Fetch(server.Url("/forever"), out, Options(), Policy(2, 10, 150));
  CHECK(result.ok && result.attempts == 2 && Since(bounded) < 1500);

  server.Script("/ra", {WithHeader(Status(429), "Retry-After: 7")});
  CHECK(Get(server.Url("/ra"), Options()).report.retry_after_ms == 7000);
  server.Script("/ra", {WithHeader(Status(503), "Retry-After: soon")});
  CHECK(Get(server.Url("/ra"), Options()).report.retry_after_ms == -1);
  server.Script("/ra", {WithHeader(Status(503), "Retry-After: Wed, 21 Oct 2015 07:28:00 GMT")});
  CHECK(Get(server.Url("/ra"), Options()).report.retry_after_ms == -1);
  server.Script("/ra", {WithHeader(Status(503), "Retry-After: 99999999999999999999")});
  CHECK(Get(server.Url("/ra"), Options()).report.retry_after_ms == -1);
  server.Script("/ra", {Status(503)});
  CHECK(Get(server.Url("/ra"), Options()).report.retry_after_ms == -1);

  const RetryPolicy policy = Policy(5, 100, 1000);
  CHECK(RetryDelayMs(policy, 1, -1) == 100);
  CHECK(RetryDelayMs(policy, 2, -1) == 200);
  CHECK(RetryDelayMs(policy, 3, -1) == 400);
  CHECK(RetryDelayMs(policy, 6, -1) == 1000);
  CHECK(RetryDelayMs(policy, 1, 600) == 600);
  CHECK(RetryDelayMs(policy, 3, 50) == 400);
  CHECK(RetryDelayMs(policy, 1, 86400000) == 1000);
  CHECK(RetryDelayMs(policy, 100, -1) == 1000);
  CHECK(RetryDelayMs(Policy(3, -5, 100), 1, -1) == 0);

  trg::HttpReport report;
  report.transient = true;
  CHECK(ShouldRetry(report));
  report.cancelled = true;
  CHECK(!ShouldRetry(report));
  report = trg::HttpReport();
  report.transient = true;
  report.failure = trg::HttpFailure::kCancelled;
  CHECK(!ShouldRetry(report));
  report.failure = trg::HttpFailure::kTotalTimeout;
  CHECK(!ShouldRetry(report));
  report.failure = trg::HttpFailure::kConnectTimeout;
  CHECK(ShouldRetry(report));
  report.transient = false;
  CHECK(!ShouldRetry(report));
}

void TestResults() {
  std::printf("result and diagnostics\n");
  Server server;
  std::string out;

  server.Script("/ok", {Body("hello")});
  TransferResult result = Fetch(server.Url("/ok"), out, Options(), Policy(3));
  CHECK(result.ok && result.failure == UpdateFailure::kNone && result.reason.empty());
  CHECK(result.host == "127.0.0.1" && result.status == 200 && result.bytes == 5 && result.expected == 5);
  CHECK(result.attempts == 1 && result.redirects == 0 && result.elapsed_ms >= 0 && !result.deadline_limited);
  CHECK(result.operation == "test" && !result.cancelled() && !result.timed_out());

  server.Script("/down", {Status(503)});
  result = Fetch(server.Url("/down"), out, Options(), Policy(3));
  CHECK(!result.ok && result.status == 503 && result.attempts == 3 && result.host == "127.0.0.1");
  CHECK(result.failure == UpdateFailure::kHttpStatus && std::string(FailureName(result.failure)) == "http_status");
  CHECK(result.reason.find("503") != std::string::npos && result.reason.size() <= 160);

  server.Script("/cut", {CutShort("0123456789")});
  result = Fetch(server.Url("/cut"), out, Options(), Policy(1));
  CHECK(!result.ok && result.bytes == 5 && result.expected == 10 && result.host == "127.0.0.1");

  server.Script("/r", {Redirect(server.Url("/ok"))});
  result = Fetch(server.Url("/r"), out, Options(), Policy(1));
  CHECK(result.ok && result.redirects == 1);

  result = Fetch("http://127.0.0.1:1/x", out, Options(), Policy(1));
  CHECK(!result.ok && result.failure != UpdateFailure::kNone && result.attempts == 1);
  result = Fetch("not a url", out, Options(), Policy(3));
  CHECK(!result.ok && result.attempts == 1 && !result.reason.empty());

  CHECK(BoundedReason(std::string(1000, 'a')).size() == 160);
  CHECK(BoundedReason("one\r\ntwo\tthree\x01" "four").find_first_of("\r\n\t\x01") == std::string::npos);
  CHECK(BoundedReason("plain text") == "plain text");

  std::set<std::string> names;
  for (int i = 0; i <= int(UpdateFailure::kNetwork); ++i) {
    const std::string name = FailureName(UpdateFailure(i));
    CHECK(!name.empty());
    names.insert(name);
  }
  CHECK(names.size() == size_t(int(UpdateFailure::kNetwork) + 1));
  std::set<std::string> transport;
  for (int i = 0; i <= int(trg::HttpFailure::kInvalidRequest); ++i) {
    const std::string name = trg::HttpFailureName(trg::HttpFailure(i));
    CHECK(!name.empty());
    transport.insert(name);
    CHECK((FailureOf(trg::HttpFailure(i)) == UpdateFailure::kNone) == (i == 0));
  }
  CHECK(transport.size() == size_t(int(trg::HttpFailure::kInvalidRequest) + 1));
  CHECK(FailureOf(trg::HttpFailure::kHttpsDowngrade) == UpdateFailure::kHttpsDowngrade);
  CHECK(FailureOf(trg::HttpFailure::kConnectTimeout) == UpdateFailure::kConnectTimeout);
  CHECK(FailureOf(trg::HttpFailure::kReceiveTimeout) == UpdateFailure::kReceiveTimeout);
  CHECK(FailureOf(trg::HttpFailure::kTotalTimeout) == UpdateFailure::kTotalTimeout);
  CHECK(FailureOf(trg::HttpFailure::kCancelled) == UpdateFailure::kCancelled);
  CHECK(FailureOf(trg::HttpFailure::kSizeLimit) == UpdateFailure::kSizeLimit);
  CHECK(FailureOf(trg::HttpFailure::kRedirectLimit) == UpdateFailure::kRedirectLimit);
  CHECK(FailureOf(trg::HttpFailure::kInvalidRedirect) == UpdateFailure::kInvalidRedirect);

  const std::string line = DescribeResult(result);
  TransferResult sample;
  sample.operation = "package";
  sample.status = 503;
  sample.attempts = 3;
  sample.bytes = 7;
  sample.expected = 9;
  sample.host = "example.test";
  sample.failure = UpdateFailure::kHttpStatus;
  sample.reason = "The server answered 503.";
  const std::string described = DescribeResult(sample);
  for (const char* part : {"update package: failed", "host=example.test", "status=503", "attempts=3", "bytes=7/9",
                           "category=http_status", "reason=\"The server answered 503.\""}) {
    CHECK(described.find(part) != std::string::npos);
  }
  CHECK(!line.empty());
}

void TestLogPrivacy() {
  std::printf("log privacy\n");
  Server first;
  Server second;
  std::vector<std::string> lines;
  trg::HttpOptions options = Options();
  options.log = [&lines](const std::string& text) { lines.push_back(text); };
  options.headers = {{"Authorization", "Bearer SECRET-TOKEN"}, {"Cookie", "session=SECRET-COOKIE"}};
  std::string out;

  first.Script("/start?sig=SECRET-SIG&token=SECRET-QUERY", {Redirect(second.Url("/end?sig=SECRET-SIG&token=SECRET-QUERY"))});
  first.Script("/start?sig=SECRET-SIG", {Redirect(second.Url("/end?sig=SECRET-SIG&token=SECRET-QUERY"))});
  second.Script("/end?sig=SECRET-SIG&token=SECRET-QUERY", {Status(503), Body("body SECRET-BODY")});
  TransferResult result = Fetch(first.Url("/start?sig=SECRET-SIG&token=SECRET-QUERY"), out, options, Policy(3));
  CHECK(result.ok);

  first.Script("/missing?token=SECRET-QUERY", {WithHeader(Status(404), "Set-Cookie: SECRET-SET")});
  result = Fetch(first.Url("/missing?token=SECRET-QUERY"), out, options, Policy(2));
  CHECK(!result.ok);
  result = Fetch(first.Url("/start?sig=SECRET-SIG"), out, options, Policy(1));

  CHECK(!lines.empty() && lines.size() < 40);
  for (const std::string& line : lines) {
    CHECK(line.find("SECRET") == std::string::npos);
    CHECK(line.find("Bearer") == std::string::npos && line.find("Authorization") == std::string::npos);
    CHECK(line.find("Cookie") == std::string::npos && line.find("sig=") == std::string::npos);
    CHECK(line.find("token=") == std::string::npos && line.find("?") == std::string::npos);
    CHECK(line.find('\n') == std::string::npos && line.size() < 400);
    CHECK(line.find(std::filesystem::temp_directory_path().string()) == std::string::npos);
  }
  const auto mentions = [&](const char* text) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  };
  CHECK(mentions("host=127.0.0.1") && mentions("status=503") && mentions("attempts=") && mentions("category="));
  CHECK(mentions("retry 2 of 3") && mentions("update test: ok") && mentions("update test: failed"));
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  struct Section {
    const char* name;
    void (*run)();
  };
  const Section sections[] = {
      {"timeouts", TestTimeouts},
      {"cancellation", TestCancellation},
      {"size", TestSizeLimits},
      {"content-length", TestContentLength},
      {"files", TestInterruptedFile},
      {"redirects", TestRedirects},
      {"origins", TestOrigins},
      {"retries", TestTransientStatuses},
      {"results", TestResults},
      {"privacy", TestLogPrivacy},
  };
  for (const Section& section : sections) {
    if (Runs(argc, argv, section.name)) {
      section.run();
    }
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
