# Mutations of the updater's production code that the updater tests must reject. Each one is compiled from a temporary copy and
# run against the test that guards the rule; nothing production is written. Test = http | network | unit.
# Edits replace the first occurrence of Old (every occurrence with All) and a text that is not found fails the run.

$dl = 'third_party\trg-launcher\src\download.cpp'
$net = 'launcher\updater\update_http.cpp'
$ctl = 'launcher\updater\update_controller.cpp'

$UpdaterMutations = @(
    @{ Name = 'connect timeout removed'; File = $dl; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'WinHttpSetTimeouts(session, options.connect_timeout_ms, options.connect_timeout_ms,'; New = 'WinHttpSetTimeouts(session, 0, 0,' },
        @{ Old = 'dog.Arm(handles.request, Watchdog::Stage::kConnect, options.connect_timeout_ms);'; New = 'dog.Arm(handles.request, Watchdog::Stage::kConnect, 3600000);' }) },
    @{ Name = 'connect and receive stalls are no longer told apart'; File = $dl; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'fire = stage_ == Stage::kConnect ? Reason::kConnectStall : Reason::kReceiveStall;'; New = 'fire = Reason::kReceiveStall;' }) },
    @{ Name = 'receive timeout removed'; File = $dl; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'options.receive_timeout_ms'; New = '3600000'; All = $true }) },
    @{ Name = 'total timeout removed'; File = $dl; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'return options.total_timeout_ms > 0 ? started'; New = 'return false ? started' }) },
    @{ Name = 'byte limit removed'; File = $dl; Test = 'http'; Section = 'size'; Edits = @(
        @{ Old = 'options.max_bytes > 0 &&'; New = 'false &&'; All = $true }) },
    @{ Name = 'Content-Length overflow accepted'; File = $dl; Test = 'http'; Section = 'size'; Edits = @(
        @{ Old = 'if (digits.size() > 15) {'; New = 'if (digits.size() > 40) {' }) },
    @{ Name = 'Content-Length mismatch accepted'; File = $dl; Test = 'http'; Section = 'content-length'; Edits = @(
        @{ Old = 'if (report.expected >= 0 && report.bytes != report.expected) {'; New = 'if (false) {' },
        @{ Old = 'if (!WinHttpQueryDataAvailable(handles.request, &available)) {'; New = 'if (!WinHttpQueryDataAvailable(handles.request, &available) && false) {' }) },
    @{ Name = 'partial body accepted'; File = $dl; Test = 'http'; Section = 'files'; Edits = @(
        @{ Old = 'if (report.expected >= 0 && report.bytes != report.expected) {'; New = 'if (false) {' },
        @{ Old = 'if (!WinHttpQueryDataAvailable(handles.request, &available)) {'; New = 'if (!WinHttpQueryDataAvailable(handles.request, &available)) { break; } if (false) {' }) },
    @{ Name = '.part promoted before validation'; File = $dl; Test = 'http'; Section = 'files'; Edits = @(
        @{ Old = 'if (error.empty() && !MoveFileOver(part, utf8_path)) {'; New = 'if (!MoveFileOver(part, utf8_path) && error.empty()) {' }) },
    @{ Name = 'old destination deleted before the transfer succeeds'; File = $dl; Test = 'http'; Section = 'files'; Edits = @(
        @{ Old = '  bool write_failed = false;'; New = '  RemoveFile(utf8_path);
  bool write_failed = false;' }) },
    @{ Name = 'a stale .part is appended to'; File = $dl; Test = 'http'; Section = 'files'; Edits = @(
        @{ Old = 'FILE* file = OpenFile(part, "wb");'; New = 'FILE* file = OpenFile(part, "ab");' }) },
    @{ Name = 'redirect limit removed'; File = $dl; Test = 'http'; Section = 'redirects'; Edits = @(
        @{ Old = 'if (report.redirects >= options.max_redirects) {'; New = 'if (false) {' }) },
    @{ Name = 'HTTPS downgrade allowed'; File = $dl; Test = 'http'; Section = 'redirects'; Edits = @(
        @{ Old = 'if (from.Secure() && !next.Secure()) {'; New = 'if (false) {' }) },
    @{ Name = 'relative redirects keep dot segments'; File = $dl; Test = 'http'; Section = 'redirects'; Edits = @(
        @{ Old = 'return base.Origin() + RemoveDotSegments(directory + location);'; New = 'return base.Origin() + directory + location;' }) },
    @{ Name = 'redirect scheme compared case-sensitively'; File = $dl; Test = 'http'; Section = 'redirects'; Edits = @(
        @{ Old = 'if (HasScheme(location)) {'; New = 'if (location.rfind("https://", 0) == 0 || location.rfind("http://", 0) == 0) {' }) },
    @{ Name = 'sensitive headers forwarded cross-origin'; File = $dl; Test = 'http'; Section = 'origins'; Edits = @(
        @{ Old = 'if (first_origin || !IsSensitiveHeader(header.name)) {'; New = 'if (true) {' }) },
    @{ Name = 'every header stripped cross-origin'; File = $dl; Test = 'http'; Section = 'origins'; Edits = @(
        @{ Old = 'if (first_origin || !IsSensitiveHeader(header.name)) {'; New = 'if (first_origin) {' }) },
    @{ Name = 'origin comparison ignores the port'; File = $dl; Test = 'http'; Section = 'origins'; Edits = @(
        @{ Old = 'return scheme == other.scheme && host == other.host && port == other.port;'; New = 'return scheme == other.scheme && host == other.host;' }) },
    @{ Name = '408 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'return status == 408 || status == 429'; New = 'return status == 429' }) },
    @{ Name = '429 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'status == 429 || status == 500'; New = 'status == 500' }) },
    @{ Name = '500 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'status == 500 || status == 502'; New = 'status == 502' }) },
    @{ Name = '502 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'status == 502 || status == 503'; New = 'status == 503' }) },
    @{ Name = '503 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'status == 503 || status == 504'; New = 'status == 504' }) },
    @{ Name = '504 no longer transient'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'status == 503 || status == 504;'; New = 'status == 503;' }) },
    @{ Name = 'permanent 404 retried'; File = $dl; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'return status == 408 ||'; New = 'return status == 404 || status == 408 ||' }) },
    @{ Name = 'Retry-After ignored'; File = $net; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'delay = std::max(delay, retry_after_ms);'; New = '' }) },
    @{ Name = 'Retry-After unbounded'; File = $net; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'return static_cast<int>(std::clamp<long long>(delay, 0, std::max(policy.max_delay_ms, 0)));'; New = 'return static_cast<int>(delay);' }) },
    @{ Name = 'retry limit removed'; File = $net; Test = 'http'; Section = 'retries'; Edits = @(
        @{ Old = 'for (int number = 1; number <= max_attempts; ++number) {'; New = 'for (int number = 1;; ++number) {' },
        @{ Old = 'if (!ShouldRetry(report) || number == max_attempts) {'; New = 'if (!ShouldRetry(report)) {' }) },
    @{ Name = 'a retry may pass the total deadline'; File = $net; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'if (bounded && Clock::now() + std::chrono::milliseconds(delay) >= deadline) {'; New = 'if (false) {' }) },
    @{ Name = 'each retry gets the whole total timeout'; File = $net; Test = 'http'; Section = 'timeouts'; Edits = @(
        @{ Old = 'options.total_timeout_ms = static_cast<int>(std::min<long long>(left, base.total_timeout_ms));'; New = 'options.total_timeout_ms = base.total_timeout_ms;' }) },
    @{ Name = 'cancellation still retries'; File = $net; Test = 'http'; Section = 'cancellation'; Edits = @(
        @{ Old = 'task && task->cancelled()'; New = 'false'; All = $true },
        @{ Old = 'return report.transient && !report.cancelled && report.failure != trg::HttpFailure::kCancelled &&'; New = 'return report.transient &&' }) },
    @{ Name = 'diagnostic loses the byte count'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'result.bytes = report.bytes;'; New = 'result.bytes = 0;' }) },
    @{ Name = 'diagnostic loses the host'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'result.host = report.host;'; New = '' }) },
    @{ Name = 'diagnostic loses the HTTP status'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'result.status = report.status;'; New = '' }) },
    @{ Name = 'diagnostic loses the category'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'result.failure = FailureOf(report.failure);'; New = 'result.failure = UpdateFailure::kNetwork;' }) },
    @{ Name = 'diagnostic loses the attempt count'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'result.attempts = number;'; New = 'result.attempts = 1;' }) },
    @{ Name = 'the reason is no longer bounded'; File = $net; Test = 'http'; Section = 'results'; Edits = @(
        @{ Old = 'if (reason.size() >= kMaxReasonBytes) {'; New = 'if (false) {' }) },
    @{ Name = 'the log carries the request headers'; File = $net; Test = 'http'; Section = 'privacy'; Edits = @(
        @{ Old = 'Log(base, DescribeResult(result));'; New = 'Log(base, DescribeResult(result) + " Authorization: " + (base.headers.empty() ? std::string() : base.headers[0].value));' }) },
    @{ Name = 'release asset matching becomes too broad'; File = 'launcher\updater\github_release.cpp'; Test = 'network'; Edits = @(
        @{ Old = 'if (asset.name == name) {'; New = 'if (asset.name.find(name) != std::string::npos) {' }) },
    @{ Name = 'a downgrade is offered as an update'; File = $ctl; Test = 'network'; Edits = @(
        @{ Old = 'if (SemanticVersion::Compare(release->version, current) <= 0) {'; New = 'if (SemanticVersion::Compare(release->version, current) == 0) {' }) },
    @{ Name = 'the same version is offered as an update'; File = $ctl; Test = 'network'; Edits = @(
        @{ Old = 'if (SemanticVersion::Compare(release->version, current) <= 0) {'; New = 'if (SemanticVersion::Compare(release->version, current) < 0) {' }) },
    @{ Name = 'draft releases are installable'; File = 'launcher\updater\github_release.cpp'; Test = 'network'; Edits = @(
        @{ Old = 'return !release.draft && !release.prerelease'; New = 'return !release.prerelease' }) },
    @{ Name = 'prereleases are installable'; File = 'launcher\updater\github_release.cpp'; Test = 'network'; Edits = @(
        @{ Old = 'return !release.draft && !release.prerelease && release.version.stable();'; New = 'return !release.draft;' }) },
    @{ Name = 'the package size limit is loosened'; File = $ctl; Test = 'network'; Edits = @(
        @{ Old = 'download_options.max_bytes = static_cast<long long>(asset.size);'; New = 'download_options.max_bytes = static_cast<long long>(asset.size) + (1ll << 20);' }) },
    @{ Name = 'a cancelled download is reported as a failure'; File = $ctl; Test = 'network'; Edits = @(
        @{ Old = 'if (state == trg::Task::State::kCancelled || failure == UpdateFailure::kCancelled) {'; New = 'if (false) {' }) },
    @{ Name = 'the package checksum is not compared'; File = $ctl; Test = 'network'; Edits = @(
        @{ Old = 'if (actual != expected) {'; New = 'if (false) {' }) },
    @{ Name = 'the Windows package name follows the Linux one'; File = 'launcher\update_config.cpp'; Test = 'network'; Edits = @(
        @{ Old = 'return PackageName(version.CoreString(), kWindowsPackageSuffix);'; New = 'return PackageName(version.CoreString(), kLinuxPackageSuffix);' }) },
    @{ Name = 'the published package name changes'; File = 'launcher\release_assets.h'; Test = 'unit'; Recompile = @('tests\update_tests.cpp', 'launcher\updater\update_paths.cpp'); Edits = @(
        @{ Old = 'kWindowsPackageSuffix = "-windows-x64.zip"'; New = 'kWindowsPackageSuffix = "-win64.zip"' }) },
    @{ Name = 'the AppImage check uses another name'; File = 'launcher\updater\update_paths.cpp'; Test = 'unit'; Edits = @(
        @{ Old = 'constexpr std::string_view kSuffix = sos::kLinuxPackageSuffix;'; New = 'constexpr std::string_view kSuffix = "-x86.AppImage";' }) }
)
