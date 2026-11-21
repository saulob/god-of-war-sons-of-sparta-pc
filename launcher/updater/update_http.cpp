#include "updater/update_http.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace trg::update {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxReasonBytes = 160;
constexpr int kSleepSliceMs = 20;

long long MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// False when the task was cancelled before the delay ended.
bool Wait(int delay_ms, trg::Task* task) {
  const Clock::time_point end = Clock::now() + std::chrono::milliseconds(delay_ms);
  while (Clock::now() < end) {
    if (task && task->cancelled()) {
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
    std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(left, std::chrono::milliseconds(kSleepSliceMs)));
  }
  return !(task && task->cancelled());
}

void Fill(TransferResult& result, const trg::HttpReport& report, const std::string& error) {
  result.status = report.status;
  result.bytes = report.bytes;
  result.expected = report.expected;
  result.redirects = report.redirects;
  if (!report.host.empty()) {
    result.host = report.host;
  }
  result.failure = FailureOf(report.failure);
  if (result.failure == UpdateFailure::kNone && !error.empty()) {
    result.failure = UpdateFailure::kNetwork;
  }
  result.reason = BoundedReason(error);
}

void Log(const trg::HttpOptions& options, const std::string& text) {
  if (options.log) {
    options.log(text);
  }
}

}  // namespace

const char* FailureName(UpdateFailure failure) {
  switch (failure) {
    case UpdateFailure::kNone: return "none";
    case UpdateFailure::kCancelled: return "cancelled";
    case UpdateFailure::kConnectTimeout: return "connect_timeout";
    case UpdateFailure::kReceiveTimeout: return "receive_timeout";
    case UpdateFailure::kTotalTimeout: return "total_timeout";
    case UpdateFailure::kHttpStatus: return "http_status";
    case UpdateFailure::kRedirectLimit: return "redirect_limit";
    case UpdateFailure::kHttpsDowngrade: return "https_downgrade";
    case UpdateFailure::kInvalidRedirect: return "invalid_redirect";
    case UpdateFailure::kAddressNotAllowed: return "address_not_allowed";
    case UpdateFailure::kSizeLimit: return "size_limit";
    case UpdateFailure::kContentLength: return "content_length";
    case UpdateFailure::kInterrupted: return "interrupted";
    case UpdateFailure::kFilesystem: return "filesystem";
    case UpdateFailure::kChecksum: return "checksum";
    case UpdateFailure::kNetwork: return "network";
  }
  return "network";
}

UpdateFailure FailureOf(trg::HttpFailure failure) {
  switch (failure) {
    case trg::HttpFailure::kNone: return UpdateFailure::kNone;
    case trg::HttpFailure::kCancelled: return UpdateFailure::kCancelled;
    case trg::HttpFailure::kConnectTimeout: return UpdateFailure::kConnectTimeout;
    case trg::HttpFailure::kReceiveTimeout: return UpdateFailure::kReceiveTimeout;
    case trg::HttpFailure::kTotalTimeout: return UpdateFailure::kTotalTimeout;
    case trg::HttpFailure::kHttpStatus: return UpdateFailure::kHttpStatus;
    case trg::HttpFailure::kRedirectLimit: return UpdateFailure::kRedirectLimit;
    case trg::HttpFailure::kHttpsDowngrade: return UpdateFailure::kHttpsDowngrade;
    case trg::HttpFailure::kInvalidRedirect: return UpdateFailure::kInvalidRedirect;
    case trg::HttpFailure::kAddressNotAllowed: return UpdateFailure::kAddressNotAllowed;
    case trg::HttpFailure::kSizeLimit: return UpdateFailure::kSizeLimit;
    case trg::HttpFailure::kContentLength: return UpdateFailure::kContentLength;
    case trg::HttpFailure::kInterrupted: return UpdateFailure::kInterrupted;
    case trg::HttpFailure::kFilesystem: return UpdateFailure::kFilesystem;
    case trg::HttpFailure::kNetwork:
    case trg::HttpFailure::kInvalidRequest: return UpdateFailure::kNetwork;
  }
  return UpdateFailure::kNetwork;
}

bool ShouldRetry(const trg::HttpReport& report) {
  return report.transient && !report.cancelled && report.failure != trg::HttpFailure::kCancelled &&
         report.failure != trg::HttpFailure::kTotalTimeout;
}

int RetryDelayMs(const RetryPolicy& policy, int failed_attempts, long long retry_after_ms) {
  long long delay = std::max(policy.base_delay_ms, 0);
  for (int i = 1; i < failed_attempts && delay < policy.max_delay_ms; ++i) {
    delay *= 2;
  }
  delay = std::max(delay, retry_after_ms);
  return static_cast<int>(std::clamp<long long>(delay, 0, std::max(policy.max_delay_ms, 0)));
}

std::string BoundedReason(std::string_view text) {
  std::string reason;
  for (const char c : text) {
    if (reason.size() >= kMaxReasonBytes) {
      break;
    }
    reason += (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) ? ' ' : c;
  }
  return reason;
}

std::string DescribeResult(const TransferResult& result) {
  return "update " + result.operation + ": " + (result.ok ? "ok" : "failed") + " host=" + result.host +
         " status=" + std::to_string(result.status) + " attempts=" + std::to_string(result.attempts) +
         " bytes=" + std::to_string(result.bytes) + "/" + std::to_string(result.expected) +
         " redirects=" + std::to_string(result.redirects) + " duration_ms=" + std::to_string(result.elapsed_ms) +
         " category=" + FailureName(result.failure) + (result.deadline_limited ? " deadline=reached" : "") +
         (result.ok || result.reason.empty() ? std::string() : " reason=\"" + result.reason + "\"");
}

TransferResult RunTransfer(const char* operation, const trg::HttpOptions& base, const RetryPolicy& policy,
                           trg::Task* task, const TransferAttempt& attempt) {
  TransferResult result;
  result.operation = operation;
  const Clock::time_point started = Clock::now();
  const bool bounded = base.total_timeout_ms > 0;
  const Clock::time_point deadline = started + std::chrono::milliseconds(std::max(base.total_timeout_ms, 0));
  const int max_attempts = std::max(policy.max_attempts, 1);

  for (int number = 1; number <= max_attempts; ++number) {
    if (task && task->cancelled()) {
      result.failure = UpdateFailure::kCancelled;
      result.reason = "Cancelled.";
      break;
    }
    trg::HttpOptions options = base;
    if (bounded) {
      const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) {
        result.failure = UpdateFailure::kTotalTimeout;
        result.reason = "The server took too long to answer.";
        break;
      }
      options.total_timeout_ms = static_cast<int>(std::min<long long>(left, base.total_timeout_ms));
    }
    trg::HttpReport report;
    options.report = &report;
    result.attempts = number;
    const std::string error = attempt(options);
    Fill(result, report, error);
    if (error.empty()) {
      result.ok = true;
      result.failure = UpdateFailure::kNone;
      result.reason.clear();
      break;
    }
    if (task && task->cancelled()) {
      result.failure = UpdateFailure::kCancelled;
      break;
    }
    if (!ShouldRetry(report) || number == max_attempts) {
      break;
    }
    const int delay = RetryDelayMs(policy, number, report.retry_after_ms);
    if (bounded && Clock::now() + std::chrono::milliseconds(delay) >= deadline) {
      result.deadline_limited = true;
      break;
    }
    Log(base, "update " + result.operation + ": retry " + std::to_string(number + 1) + " of " +
                  std::to_string(max_attempts) + " in " + std::to_string(delay) + " ms after " +
                  FailureName(result.failure));
    if (!Wait(delay, task)) {
      result.failure = UpdateFailure::kCancelled;
      result.reason = "Cancelled.";
      break;
    }
  }
  result.elapsed_ms = MillisecondsSince(started);
  Log(base, DescribeResult(result));
  return result;
}

TransferResult FetchText(const char* operation, const std::string& url, std::string& out, trg::Task* task,
                         const trg::HttpOptions& options, const RetryPolicy& policy) {
  return RunTransfer(operation, options, policy, task,
                     [&](const trg::HttpOptions& attempt) { return trg::HttpGet(url, out, task, attempt); });
}

TransferResult FetchFile(const char* operation, const std::string& url, const std::string& utf8_path, trg::Task& task,
                         const trg::HttpOptions& options, const RetryPolicy& policy) {
  return RunTransfer(operation, options, policy, &task, [&](const trg::HttpOptions& attempt) {
    return trg::DownloadFile(task, url, utf8_path, attempt);
  });
}

}  // namespace trg::update
