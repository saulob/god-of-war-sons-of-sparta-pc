#pragma once

// A scripted HTTP/1.1 server on the loopback interface for the updater tests: one connection at a time, no Internet.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace update_test {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
constexpr int kNoSignal = 0;
inline void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
constexpr int kNoSignal = MSG_NOSIGNAL;
inline void CloseSocket(Socket s) { close(s); }
#endif

inline bool Readable(Socket socket, int milliseconds) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
  return select(int(socket) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

struct Step {
  enum class Kind { kBody, kStatus, kRedirect, kCutShort, kStall, kHang, kDrip };
  Kind kind = Kind::kBody;
  int status = 200;
  std::string body;
  std::string location;
  std::vector<std::string> headers;
  long long declared = -1;       // Content-Length to announce; -1 announces the real body size
  bool close_delimited = false;  // announce no length at all
  bool chunked = false;
  int delay_ms = 0;         // wait before answering
  std::string length_text;  // Content-Length text to send as it is
};

inline Step Make(Step::Kind kind, int status, std::string body = {}, std::string location = {}) {
  Step step;
  step.kind = kind;
  step.status = status;
  step.body = std::move(body);
  step.location = std::move(location);
  return step;
}
inline Step Body(std::string body) { return Make(Step::Kind::kBody, 200, std::move(body)); }
inline Step Status(int status) { return Make(Step::Kind::kStatus, status, "unavailable"); }
inline Step Redirect(std::string location, int status = 302) {
  return Make(Step::Kind::kRedirect, status, {}, std::move(location));
}
inline Step CutShort(std::string body) { return Make(Step::Kind::kCutShort, 200, std::move(body)); }
inline Step Stall(std::string body) { return Make(Step::Kind::kStall, 200, std::move(body)); }
inline Step Hang() { return Make(Step::Kind::kHang, 200); }
inline Step Drip(long long declared) {
  Step step = Make(Step::Kind::kDrip, 200);
  step.declared = declared;
  return step;
}
inline Step Delayed(Step step, int milliseconds) {
  step.delay_ms = milliseconds;
  return step;
}
inline Step LengthText(Step step, std::string text) {
  step.length_text = std::move(text);
  return step;
}

inline Step WithHeader(Step step, std::string line) {
  step.headers.push_back(std::move(line));
  return step;
}
inline Step Declaring(Step step, long long length) {
  step.declared = length;
  return step;
}
inline Step WithoutLength(Step step) {
  step.close_delimited = true;
  return step;
}
inline Step Chunked(Step step) {
  step.chunked = true;
  return step;
}

// A path's steps answer its requests in order; the last one repeats.
class Server {
 public:
  Server() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener_, 8);
    socklen_t length = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }
  ~Server() {
    stop_ = true;
    thread_.join();
    CloseSocket(listener_);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  unsigned short port() const { return port_; }
  std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  bool Allows(const std::string& url) const { return url.rfind(Url("/"), 0) == 0; }

  void Script(const std::string& path, std::vector<Step> steps) {
    std::lock_guard<std::mutex> lock(mutex_);
    scripts_[path] = std::move(steps);
    counts_[path] = 0;
    requests_[path].clear();
  }
  int Requests(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_[path];
  }
  // The raw request text, headers included, of the n-th request to a path.
  std::string Request(const std::string& path, size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    return n < requests_[path].size() ? requests_[path][n] : std::string();
  }

 private:
  void Run() {
    while (!stop_) {
      if (!Readable(listener_, 50)) {
        continue;
      }
      const Socket client = accept(listener_, nullptr, nullptr);
      if (client != kNoSocket) {
        Serve(client);
        CloseSocket(client);
      }
    }
  }

  void Send(Socket client, const std::string& text) { send(client, text.data(), int(text.size()), kNoSignal); }

  void Serve(Socket client) {
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && Readable(client, 2000)) {
      const int count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        return;
      }
      request.append(buffer, size_t(count));
    }
    const size_t start = request.find(' ');
    const size_t end = request.find(' ', start + 1);
    if (start == std::string::npos || end == std::string::npos) {
      return;
    }
    const std::string path = request.substr(start + 1, end - start - 1);

    Step step = Status(404);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto script = scripts_.find(path);
      if (script != scripts_.end()) {
        const int index = counts_[path]++;
        step = script->second[std::min<size_t>(size_t(index), script->second.size() - 1)];
      }
      requests_[path].push_back(request);
    }

    std::string extra;
    for (const std::string& line : step.headers) {
      extra += line + "\r\n";
    }
    const auto head = [&](int status, size_t length) {
      std::string lines = "HTTP/1.1 " + std::to_string(status) + " Scripted\r\n";
      if (step.chunked) {
        lines += "Transfer-Encoding: chunked\r\n";
      } else if (!step.length_text.empty()) {
        lines += "Content-Length: " + step.length_text + "\r\n";
      } else if (!step.close_delimited) {
        lines += "Content-Length: " + (step.declared >= 0 ? std::to_string(step.declared) : std::to_string(length)) +
                 "\r\n";
      }
      return lines + "Connection: close\r\n" + extra + "\r\n";
    };
    const auto payload = [&](const std::string& body) {
      if (!step.chunked) {
        return body;
      }
      char size[32];
      std::snprintf(size, sizeof(size), "%zx\r\n", body.size());
      return std::string(size) + body + "\r\n0\r\n\r\n";
    };
    for (int waited = 0; waited < step.delay_ms && !stop_; waited += 10) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    switch (step.kind) {
      case Step::Kind::kBody:
      case Step::Kind::kStatus:
        Send(client, head(step.status, step.body.size()) + payload(step.body));
        break;
      case Step::Kind::kRedirect:
        extra += "Location: " + step.location + "\r\n";
        Send(client, head(step.status, 0));
        break;
      case Step::Kind::kCutShort:
        Send(client, head(200, step.body.size()) + step.body.substr(0, step.body.size() / 2));
        break;
      case Step::Kind::kStall:
        Send(client, head(200, step.body.size()) + step.body.substr(0, step.body.size() / 2));
        while (!stop_ && !Readable(client, 50)) {
        }
        break;
      case Step::Kind::kDrip:
        Send(client, head(200, 0));
        while (!stop_ && !Readable(client, 40)) {
          Send(client, "x");
        }
        break;
      case Step::Kind::kHang:
        while (!stop_ && !Readable(client, 50)) {
        }
        break;
    }
  }

  Socket listener_ = kNoSocket;
  unsigned short port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::map<std::string, std::vector<Step>> scripts_;
  std::map<std::string, int> counts_;
  std::map<std::string, std::vector<std::string>> requests_;
};

}  // namespace update_test
