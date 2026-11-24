// The launcher's short-lived session with the bug report service: connect, send numbered requests, close. Not
// thread-safe; blocks, so run it off the UI thread.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "net/json.h"
#include "net/websocket.h"

namespace sos::net {

inline constexpr char kLinkLost[] = "link_lost";
inline constexpr char kUnreachable[] = "unreachable";

class ReportSession {
public:
    ~ReportSession();

    // Opens the socket without sending anything; the client id (when given) is part of the upgrade address and only
    // feeds the service's abuse quota. False with the reason when the service cannot be reached.
    bool connect(const std::string& url, std::string* error, int timeout_ms = 10000, const std::string& client_id = {});
    // The answer, or nullopt with `code` set ("timeout", kLinkLost or the service's error code).
    std::optional<Json> call(const std::string& op, Json body, std::string* code);
    bool send_binary(const uint8_t* data, size_t size);
    bool connected() const { return socket_ != nullptr; }
    void close();
    const Json& last_reply() const { return last_reply_; }

private:
    std::unique_ptr<WebSocket> socket_;
    uint32_t seq_ = 0;
    Json last_reply_;
};

} // namespace sos::net
