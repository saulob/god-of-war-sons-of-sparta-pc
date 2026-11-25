#include "net/report_link.h"

#include <utility>

namespace sos::net {
namespace {

std::string AddressWithClient(std::string url, const std::string& client_id) {
    if (client_id.empty() || url.find('?') != std::string::npos) return url;
    const size_t scheme = url.find("://");
    const size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path == std::string::npos) url += "/";
    return url + "?client=" + client_id;
}

} // namespace

ReportSession::~ReportSession() { close(); }

bool ReportSession::connect(const std::string& url, std::string* error, int timeout_ms, const std::string& client_id) {
    close();
    socket_ = WebSocket::connect(AddressWithClient(url, client_id), timeout_ms, error);
    seq_ = 0;
    last_reply_ = Json();
    return socket_ != nullptr;
}

std::optional<Json> ReportSession::call(const std::string& op, Json body, std::string* code) {
    if (!socket_) {
        if (code) *code = kUnreachable;
        return std::nullopt;
    }
    if (!body.is_object()) body = Json::Object();
    Json envelope = Json::Object();
    envelope.set("op", op);
    envelope.set("seq", int64_t(++seq_));
    for (const auto& [key, value] : body.members()) envelope.set(key, value);
    if (!socket_->send_text(envelope.dump())) {
        if (code) *code = kLinkLost;
        close();
        return std::nullopt;
    }
    for (;;) {
        std::string text;
        bool binary = false;
        if (!socket_->receive(text, binary)) {
            if (code) *code = kLinkLost;
            close();
            return std::nullopt;
        }
        if (binary) continue;
        const std::optional<Json> reply = Json::parse(text);
        if (!reply || !reply->is_object()) {
            if (code) *code = "bad_reply";
            close();
            return std::nullopt;
        }
        if ((*reply)["re"].as_int(-1) != int64_t(seq_)) continue;
        last_reply_ = *reply;
        const std::string result = reply->has("code") ? (*reply)["code"].as_string() : std::string("bad_reply");
        if (code) *code = result;
        if (result != "ok") return std::nullopt;
        return reply;
    }
}

bool ReportSession::send_binary(const uint8_t* data, size_t size) {
    return socket_ && socket_->send_binary(data, size);
}

void ReportSession::close() {
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
}

} // namespace sos::net
