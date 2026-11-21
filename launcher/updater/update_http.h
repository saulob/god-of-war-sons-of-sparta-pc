#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <trg/download.h>
#include <trg/platform.h>

namespace trg::update {

inline constexpr long long kMaxReleaseJsonBytes = 4ll << 20;
inline constexpr long long kMaxChecksumBytes = 64ll << 10;
inline constexpr long long kMaxPackageBytes = 1ll << 30;

inline constexpr int kDefaultConnectTimeoutMs = 15000;
inline constexpr int kDefaultReceiveTimeoutMs = 30000;
inline constexpr int kCheckTimeoutMs = 30000;
inline constexpr int kChecksumTimeoutMs = 20000;
inline constexpr int kDownloadTimeoutMs = 30 * 60 * 1000;

// Attempts and delays of a transfer; the total timeout of the transfer bounds all of them together.
struct RetryPolicy {
  int max_attempts = 3;
  int base_delay_ms = 1000;
  int max_delay_ms = 10000;
};

enum class UpdateFailure {
  kNone,
  kCancelled,
  kConnectTimeout,
  kReceiveTimeout,
  kTotalTimeout,
  kHttpStatus,
  kRedirectLimit,
  kHttpsDowngrade,
  kInvalidRedirect,
  kAddressNotAllowed,
  kSizeLimit,
  kContentLength,
  kInterrupted,
  kFilesystem,
  kChecksum,
  kNetwork,
};

const char* FailureName(UpdateFailure failure);
UpdateFailure FailureOf(trg::HttpFailure failure);

// What a whole logical transfer did, including its retries.
struct TransferResult {
  std::string operation;
  bool ok = false;
  UpdateFailure failure = UpdateFailure::kNone;
  int status = 0;
  long long bytes = 0;
  long long expected = -1;
  std::string host;
  int attempts = 0;
  int redirects = 0;
  long long elapsed_ms = 0;
  bool deadline_limited = false;
  std::string reason;

  bool cancelled() const { return failure == UpdateFailure::kCancelled; }
  bool timed_out() const {
    return failure == UpdateFailure::kConnectTimeout || failure == UpdateFailure::kReceiveTimeout ||
           failure == UpdateFailure::kTotalTimeout;
  }
};

// Only a failure that can pass by itself is tried again, and never a cancelled or out-of-time one.
bool ShouldRetry(const trg::HttpReport& report);

// Exponential backoff from the base delay, or the server's Retry-After when it is longer; never above the maximum.
int RetryDelayMs(const RetryPolicy& policy, int failed_attempts, long long retry_after_ms);

// One line of printable text of bounded length.
std::string BoundedReason(std::string_view text);

// Host, status, attempts, bytes, duration and category; never an address, a header or a body.
std::string DescribeResult(const TransferResult& result);

using TransferAttempt = std::function<std::string(const trg::HttpOptions& options)>;

// Runs `attempt` until it succeeds, fails for good, runs out of attempts or the total timeout of `base` passes.
// Each attempt gets the time that is left. `task` stops the delays between attempts when cancelled.
TransferResult RunTransfer(const char* operation, const trg::HttpOptions& base, const RetryPolicy& policy,
                           trg::Task* task, const TransferAttempt& attempt);

TransferResult FetchText(const char* operation, const std::string& url, std::string& out, trg::Task* task,
                         const trg::HttpOptions& options, const RetryPolicy& policy);

// Every attempt restarts `<path>.part`; the file at `path` changes only when a whole transfer succeeded.
TransferResult FetchFile(const char* operation, const std::string& url, const std::string& utf8_path, trg::Task& task,
                         const trg::HttpOptions& options, const RetryPolicy& policy);

}  // namespace trg::update
