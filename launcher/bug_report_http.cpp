#include "bug_report_http.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>

#include "updater/json.h"
#include "utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sos {
namespace fs = std::filesystem;
namespace {

constexpr char kClientIdFile[] = "bug_report_client.txt";
constexpr size_t kReceiptHexDigits = 64;
constexpr size_t kMaxResponseBytes = 256 * 1024;

bool IsReceiptHex(std::string_view text) {
    return text.size() == kReceiptHexDigits &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string TrimTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::string LoadOrCreateClientId(const fs::path& user_dir) {
    const auto valid = [](const std::string& id) {
        return id.size() >= 16 && id.size() <= 64 &&
               std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const fs::path path = user_dir / kClientIdFile;
    {
        std::ifstream in(path);
        std::string stored;
        if (std::getline(in, stored) && valid(stored)) return stored;
    }
    std::random_device rd;
    char hex[33] = {};
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", rd() & 0xffu);
    std::string generated(hex);
    std::error_code ec;
    fs::create_directories(user_dir, ec);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << generated << "\n";
    return generated;
}

struct ParsedUrl {
    bool https = false;
    std::string host;
    uint16_t port = 0;
    std::string path;
    bool valid = false;
};

ParsedUrl ParseUrl(std::string_view url) {
    ParsedUrl out;
    if (url.starts_with("https://")) {
        out.https = true;
        out.port = 443;
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        out.https = false;
        out.port = 80;
        url.remove_prefix(7);
    } else {
        return out;
    }
    const size_t slash = url.find('/');
    const std::string_view authority = slash == std::string_view::npos ? url : url.substr(0, slash);
    out.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    if (authority.empty()) return out;
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        out.host = std::string(authority.substr(0, colon));
        const std::string_view port_str = authority.substr(colon + 1);
        if (port_str.empty()) return out;
        unsigned long port_val = 0;
        for (char c : port_str) {
            if (c < '0' || c > '9') return out;
            port_val = port_val * 10 + unsigned(c - '0');
            if (port_val > 65535) return out;
        }
        if (port_val == 0) return out;
        out.port = uint16_t(port_val);
    } else {
        out.host = std::string(authority);
    }
    out.valid = !out.host.empty();
    return out;
}

#ifdef _WIN32
std::wstring ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(size_t(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), need);
    return out;
}

HttpResponse SendWinHttp(const HttpRequest& request) {
    HttpResponse response;
    const ParsedUrl parsed = ParseUrl(request.url);
    if (!parsed.valid) {
        response.error = "invalid url";
        return response;
    }
    HINTERNET session = WinHttpOpen(L"SonsOfSpartaPC-BugReport/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = "WinHttpOpen failed";
        return response;
    }
    WinHttpSetTimeouts(session, request.timeout_ms, request.timeout_ms, request.timeout_ms, request.timeout_ms);
    HINTERNET connect = WinHttpConnect(session, ToWide(parsed.host).c_str(), parsed.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        response.error = "WinHttpConnect failed";
        return response;
    }
    const DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(connect, ToWide(request.method).c_str(), ToWide(parsed.path).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        response.error = "WinHttpOpenRequest failed";
        return response;
    }
    for (const auto& [name, value] : request.headers) {
        const std::wstring header = ToWide(name + ": " + value);
        WinHttpAddRequestHeaders(req, header.c_str(), DWORD(header.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    const LPVOID body_ptr = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : LPVOID(const_cast<uint8_t*>(request.body.data()));
    const DWORD body_len = DWORD(request.body.size());
    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body_ptr, body_len, body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        response.error = "WinHttp request failed";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    response.status = int(status);
    response.transport_ok = true;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
        const size_t chunk = std::min<size_t>(available, kMaxResponseBytes - response.body.size());
        if (chunk == 0) break;
        const size_t prev = response.body.size();
        response.body.resize(prev + chunk);
        DWORD read = 0;
        if (!WinHttpReadData(req, response.body.data() + prev, DWORD(chunk), &read) || read == 0) {
            response.body.resize(prev);
            break;
        }
        response.body.resize(prev + read);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}
#else
HttpResponse SendPosixHttp(const HttpRequest& request) {
    HttpResponse response;
    const ParsedUrl parsed = ParseUrl(request.url);
    if (!parsed.valid) {
        response.error = "invalid url";
        return response;
    }
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(parsed.port);
    if (getaddrinfo(parsed.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        response.error = "resolve failed";
        return response;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && request.timeout_ms > 0) {
        struct timeval tv{};
        tv.tv_sec = request.timeout_ms / 1000;
        tv.tv_usec = (request.timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        response.error = "connect failed";
        return response;
    }
    freeaddrinfo(res);

    SSL_CTX* ssl_ctx = nullptr;
    SSL* ssl = nullptr;
    const auto cleanup = [&]() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ssl_ctx) {
            SSL_CTX_free(ssl_ctx);
            ssl_ctx = nullptr;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };
    if (parsed.https) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx || SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
            cleanup();
            response.error = "tls init failed";
            return response;
        }
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            cleanup();
            response.error = "tls session failed";
            return response;
        }
        SSL_set_tlsext_host_name(ssl, parsed.host.c_str());
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        if (X509_VERIFY_PARAM_set1_host(param, parsed.host.c_str(), parsed.host.size()) != 1) {
            cleanup();
            response.error = "tls hostname setup failed";
            return response;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) {
            cleanup();
            response.error = "tls handshake failed";
            return response;
        }
    }

    std::string head = request.method + " " + parsed.path + " HTTP/1.1\r\nHost: " + parsed.host +
                       "\r\nConnection: close\r\nContent-Length: " + std::to_string(request.body.size()) + "\r\n";
    for (const auto& [k, v] : request.headers) head += k + ": " + v + "\r\n";
    head += "\r\n";
    const auto send_all = [&](const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
            const ssize_t n = ssl ? SSL_write(ssl, p, int(std::min<size_t>(len, 16384))) : send(fd, p, len, 0);
            if (n <= 0) return false;
            p += n;
            len -= size_t(n);
        }
        return true;
    };
    if (!send_all(head.data(), head.size()) || (!request.body.empty() && !send_all(request.body.data(), request.body.size()))) {
        cleanup();
        response.error = "send failed";
        return response;
    }
    std::string raw;
    char buf[4096];
    for (;;) {
        const ssize_t n = ssl ? SSL_read(ssl, buf, sizeof(buf)) : recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (raw.size() + size_t(n) > kMaxResponseBytes) break;
        raw.append(buf, size_t(n));
    }
    cleanup();
    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        response.error = "bad response";
        return response;
    }
    int status = 0;
    if (std::sscanf(raw.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
        response.error = "bad status";
        return response;
    }
    response.transport_ok = true;
    response.status = status;
    response.body = raw.substr(sep + 4);
    return response;
}
#endif

std::string JsonStringField(const trg::update::json::Value& obj, std::string_view key) {
    const auto* value = obj.Find(key);
    return value && value->IsString() ? value->AsString() : std::string();
}

int64_t JsonIntField(const trg::update::json::Value& obj, std::string_view key, int64_t fallback = 0) {
    const auto* value = obj.Find(key);
    if (!value || !value->IsNumber()) return fallback;
    const auto i = value->AsInt64();
    return i ? *i : fallback;
}

bool JsonBoolField(const trg::update::json::Value& obj, std::string_view key, bool fallback = false) {
    const auto* value = obj.Find(key);
    return value && value->IsBool() ? value->AsBool() : fallback;
}

enum class DumpStep { kDone, kRetry, kStop };

DumpStep UploadDumpPass(const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                        const CrashDump& dump, const HttpTransport& send, const BugReportProgressSink& progress) {
    const auto report = [&](uint64_t sent) {
        if (progress) progress({BugReportProgress::Kind::kDumpBytes, ticket, sent, dump.bytes});
    };
    std::error_code ec;
    if (fs::file_size(dump.path, ec) != dump.bytes || ec) return DumpStep::kStop;

    const std::string base = TrimTrailingSlash(config.endpoint_url);
    const HttpResponse status_res = send({"GET", base + "/api/reports/" + ticket + "/dump",
                                          {{"X-SOS-Receipt", receipt}}, {}, config.timeout_ms});
    if (!status_res.transport_ok) return DumpStep::kRetry;
    if (status_res.status == 400 || status_res.status == 404 || status_res.status == 413) return DumpStep::kStop;
    if (status_res.status < 200 || status_res.status >= 300) return DumpStep::kRetry;

    const auto state = trg::update::json::Parse(status_res.body);
    if (!state || !state->IsObject()) return DumpStep::kRetry;
    if (uint64_t(JsonIntField(*state, "bytes", -1)) != dump.bytes) return DumpStep::kStop;
    if (JsonBoolField(*state, "complete", false)) return DumpStep::kDone;

    uint64_t at = uint64_t(std::max<int64_t>(0, JsonIntField(*state, "stored", 0)));
    if (at > dump.bytes) return DumpStep::kStop;
    report(at);
    const size_t chunk_limit = size_t(std::clamp<int64_t>(JsonIntField(*state, "chunk_bytes", int64_t(kDumpChunkBytes)),
                                                          1024, int64_t(kDumpChunkBytes)));

    std::ifstream in(dump.path, std::ios::binary);
    if (!in) return DumpStep::kStop;
    in.seekg(std::streamoff(at));
    std::vector<uint8_t> chunk;
    while (at < dump.bytes) {
        const size_t want = size_t(std::min<uint64_t>(chunk_limit, dump.bytes - at));
        chunk.resize(want);
        in.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(want));
        if (size_t(in.gcount()) != want) return DumpStep::kStop;

        const HttpResponse put_res = send({"PUT",
                                           base + "/api/reports/" + ticket + "/dump/chunk?offset=" + std::to_string(at),
                                           {{"X-SOS-Receipt", receipt}, {"Content-Type", "application/octet-stream"}},
                                           chunk, config.timeout_ms});
        if (!put_res.transport_ok) return DumpStep::kRetry;
        if (put_res.status == 400 || put_res.status == 404 || put_res.status == 413) return DumpStep::kStop;
        if (put_res.status == 409) return DumpStep::kRetry;
        if (put_res.status < 200 || put_res.status >= 300) return DumpStep::kRetry;
        at += want;
        report(at);
    }

    const HttpResponse done_res = send({"POST", base + "/api/reports/" + ticket + "/dump/complete",
                                        {{"X-SOS-Receipt", receipt}}, {}, config.timeout_ms});
    if (!done_res.transport_ok) return DumpStep::kRetry;
    if (done_res.status >= 200 && done_res.status < 300) return DumpStep::kDone;
    if (done_res.status == 400 || done_res.status == 404) return DumpStep::kStop;
    return DumpStep::kRetry;
}

} // namespace

BugReportServiceConfig ResolveBugReportConfig(const fs::path& user_dir) {
    BugReportServiceConfig config;
    if (const char* env = std::getenv("SOS_BUG_REPORT_URL"); env && *env) {
        config.endpoint_url = TrimTrailingSlash(env);
    }
    config.client_id = LoadOrCreateClientId(user_dir);
    return config;
}

HttpResponse SendHttpRequest(const HttpRequest& request) {
#ifdef _WIN32
    return SendWinHttp(request);
#else
    return SendPosixHttp(request);
#endif
}

CrashUpload UploadCrashDump(const BugReportServiceConfig& config, const std::string& ticket, const std::string& receipt,
                            const CrashDump& dump, const HttpTransport& transport, const BugReportProgressSink& progress) {
    if (config.endpoint_url.empty() || ticket.empty() || !IsReceiptHex(receipt)) return CrashUpload::kFailed;
    const HttpTransport send = transport ? transport : HttpTransport(SendHttpRequest);
    for (int attempt = 0; attempt < kDumpUploadAttempts; ++attempt) {
        if (attempt > 0 && config.retry_pause_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config.retry_pause_ms * attempt));
        }
        const DumpStep step = UploadDumpPass(config, ticket, receipt, dump, send, progress);
        if (step == DumpStep::kDone) return CrashUpload::kUploaded;
        if (step == DumpStep::kStop) break;
    }
    return CrashUpload::kFailed;
}

BugReportReceipt FileBugReport(const BugReportServiceConfig& config, const BugReportPackage& package,
                               const HttpTransport& transport, const BugReportProgressSink& progress) {
    BugReportReceipt receipt;
    if (config.endpoint_url.empty()) {
        receipt.outcome = BugReportOutcome::kUnreachable;
        return receipt;
    }
    const HttpTransport send = transport ? transport : HttpTransport(SendHttpRequest);
    const std::string json_text = BugReportRequestJson(package);
    std::vector<uint8_t> body(json_text.begin(), json_text.end());
    std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json; charset=utf-8"}};
    if (!config.client_id.empty()) headers.emplace_back("X-SOS-Client", config.client_id);

    const HttpResponse response = send({"POST", TrimTrailingSlash(config.endpoint_url) + "/api/reports",
                                        std::move(headers), std::move(body), config.timeout_ms});
    if (!response.transport_ok) {
        receipt.outcome = BugReportOutcome::kUnreachable;
        return receipt;
    }
    const auto parsed = trg::update::json::Parse(response.body);
    const std::string error_code = parsed && parsed->IsObject() ? JsonStringField(*parsed, "error") : std::string();

    if (response.status == 429) {
        receipt.outcome = error_code == "daily_limit" ? BugReportOutcome::kDailyLimit : BugReportOutcome::kRecentLimit;
        return receipt;
    }
    if (response.status == 400 || response.status == 413) {
        receipt.outcome = BugReportOutcome::kRefused;
        return receipt;
    }
    if (response.status < 200 || response.status >= 300 || !parsed || !parsed->IsObject()) {
        receipt.outcome = BugReportOutcome::kServiceError;
        return receipt;
    }

    receipt.ticket = JsonStringField(*parsed, "ticket");
    const std::string cred = JsonStringField(*parsed, "receipt");
    if (receipt.ticket.empty()) {
        receipt.outcome = BugReportOutcome::kServiceError;
        return receipt;
    }
    receipt.outcome = BugReportOutcome::kSent;
    receipt.credential = IsReceiptHex(cred) ? cred : std::string();
    receipt.submitted_at = JsonIntField(*parsed, "submitted_at", 0);
    if (progress) progress({BugReportProgress::Kind::kTextAccepted, receipt.ticket, 0, 0});

    if (package.crash_dump) {
        const std::string dump_state = JsonStringField(*parsed, "dump");
        receipt.crash = (receipt.credential.empty() || dump_state != "expected")
                            ? CrashUpload::kFailed
                            : UploadCrashDump(config, receipt.ticket, receipt.credential, *package.crash_dump, send, progress);
    } else if (package.crash_dump_requested) {
        receipt.crash = CrashUpload::kFailed;
    }
    return receipt;
}

} // namespace sos
