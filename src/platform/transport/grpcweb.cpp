// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/transport/grpcweb.h>

#include <common/url.h>
#include <platform/transport/tls.h>
#include <util/string.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <optional>

namespace platform::transport {

namespace {

//! Wrap a protobuf message in a single gRPC-Web data frame.
std::vector<uint8_t> FrameMessage(const std::vector<uint8_t>& msg)
{
    std::vector<uint8_t> frame;
    frame.reserve(5 + msg.size());
    frame.push_back(0x00); // data frame, not compressed
    const uint32_t len = static_cast<uint32_t>(msg.size());
    frame.push_back(static_cast<uint8_t>(len >> 24));
    frame.push_back(static_cast<uint8_t>(len >> 16));
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len));
    frame.insert(frame.end(), msg.begin(), msg.end());
    return frame;
}

std::string ToLower(std::string s)
{
    // HTTP/gRPC-Web header names are ASCII; use a locale-independent fold so
    // header matching cannot vary with the process locale.
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; });
    return s;
}

std::optional<std::string> HeaderValue(const std::string& headers, const std::string& name)
{
    const std::string lower_headers{ToLower(headers)};
    const std::string needle{ToLower(name) + ":"};
    size_t pos{lower_headers.find(needle)};
    while (pos != std::string::npos && pos != 0 && lower_headers[pos - 1] != '\n') {
        pos = lower_headers.find(needle, pos + 1);
    }
    if (pos == std::string::npos) return std::nullopt;
    size_t start{pos + needle.size()};
    while (start < headers.size() && (headers[start] == ' ' || headers[start] == '\t'))
        ++start;
    const size_t line_end{headers.find("\r\n", start)};
    return headers.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
}

bool ParseGrpcStatus(const std::string& value, int& status)
{
    const char* first{value.data()};
    const char* last{first + value.size()};
    const auto [ptr, ec] = std::from_chars(first, last, status);
    return !value.empty() && ec == std::errc{} && ptr == last && status >= 0 && status <= 16;
}

bool Dechunk(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out, std::string& error)
{
    static constexpr uint8_t CRLF[]{'\r', '\n'};
    static constexpr uint8_t HEADER_END[]{'\r', '\n', '\r', '\n'};
    size_t pos{0};
    while (pos < payload.size()) {
        const auto line_end{std::search(payload.begin() + pos, payload.end(), std::begin(CRLF), std::end(CRLF))};
        if (line_end == payload.end()) {
            error = "malformed chunk size";
            return false;
        }
        std::string hexlen(payload.begin() + pos, line_end);
        if (const size_t ext{hexlen.find(';')}; ext != std::string::npos) hexlen.resize(ext);
        size_t chunk_len{0};
        const char* first{hexlen.data()};
        const char* last{first + hexlen.size()};
        const auto [ptr, ec] = std::from_chars(first, last, chunk_len, 16);
        if (hexlen.empty() || ec != std::errc{} || ptr != last) {
            error = "invalid chunk size";
            return false;
        }
        pos = static_cast<size_t>(line_end - payload.begin()) + 2;
        if (chunk_len == 0) {
            if (payload.size() - pos == 2 && payload[pos] == '\r' && payload[pos + 1] == '\n') return true;
            const auto trailer_end{
                std::search(payload.begin() + pos, payload.end(), std::begin(HEADER_END), std::end(HEADER_END))};
            if (trailer_end != payload.end() && trailer_end + std::size(HEADER_END) == payload.end()) return true;
            error = "malformed final chunk";
            return false;
        }
        if (chunk_len > payload.size() - pos) {
            error = "truncated chunk";
            return false;
        }
        out.insert(out.end(), payload.begin() + pos, payload.begin() + pos + chunk_len);
        pos += chunk_len;
        if (payload.size() - pos < 2 || payload[pos] != '\r' || payload[pos + 1] != '\n') {
            error = "missing chunk terminator";
            return false;
        }
        pos += 2;
    }
    error = "missing final chunk";
    return false;
}

} // namespace

GrpcCallResult ParseGrpcWebResponse(Span<const uint8_t> response)
{
    GrpcCallResult result;
    static constexpr uint8_t HEADER_END[]{'\r', '\n', '\r', '\n'};
    const auto header_end{std::search(response.begin(), response.end(), std::begin(HEADER_END), std::end(HEADER_END))};
    if (header_end == response.end()) {
        result.transport_error = "malformed HTTP response";
        return result;
    }
    const std::string headers(response.begin(), header_end);
    std::vector<uint8_t> payload(header_end + std::size(HEADER_END), response.end());

    const size_t eol{headers.find("\r\n")};
    const std::string status_line{headers.substr(0, eol)};
    const size_t code_start{status_line.find(' ')};
    const size_t code_end{code_start == std::string::npos ? std::string::npos : status_line.find(' ', code_start + 1)};
    if (status_line.rfind("HTTP/1.", 0) != 0 || code_start == std::string::npos ||
        status_line.substr(code_start + 1, code_end - code_start - 1) != "200") {
        result.transport_error = "HTTP error: " + status_line;
        return result;
    }

    if (const auto transfer_encoding{HeaderValue(headers, "transfer-encoding")};
        transfer_encoding && ToLower(*transfer_encoding).find("chunked") != std::string::npos) {
        std::vector<uint8_t> dechunked;
        if (!Dechunk(payload, dechunked, result.transport_error)) return result;
        payload = std::move(dechunked);
    }

    bool saw_status{false};
    if (const auto status{HeaderValue(headers, "grpc-status")}) {
        if (!ParseGrpcStatus(*status, result.grpc_status)) {
            result.transport_error = "invalid grpc-status header";
            return result;
        }
        saw_status = true;
    }
    if (const auto message{HeaderValue(headers, "grpc-message")}) result.grpc_message = urlDecode(*message);

    size_t pos{0};
    bool saw_data{false};
    bool saw_trailers{false};
    while (pos < payload.size()) {
        if (saw_trailers) {
            result.transport_error = "data after gRPC-Web trailers";
            return result;
        }
        if (payload.size() - pos < 5) {
            result.transport_error = "truncated gRPC-Web frame header";
            return result;
        }
        const uint8_t flags{payload[pos]};
        const uint32_t len = (static_cast<uint32_t>(payload[pos + 1]) << 24) |
                             (static_cast<uint32_t>(payload[pos + 2]) << 16) |
                             (static_cast<uint32_t>(payload[pos + 3]) << 8) | static_cast<uint32_t>(payload[pos + 4]);
        pos += 5;
        if (len > payload.size() - pos) {
            result.transport_error = "truncated gRPC-Web frame";
            return result;
        }
        const Span<const uint8_t> frame{payload.data() + pos, len};
        pos += len;

        if (flags == 0x80) {
            const std::string trailers(frame.begin(), frame.end());
            const auto status{HeaderValue(trailers, "grpc-status")};
            if (!status || !ParseGrpcStatus(*status, result.grpc_status)) {
                result.transport_error = "missing or invalid grpc-status trailer";
                return result;
            }
            saw_status = true;
            saw_trailers = true;
            if (const auto message{HeaderValue(trailers, "grpc-message")}) result.grpc_message = urlDecode(*message);
        } else if (flags == 0x00) {
            if (saw_data) {
                result.transport_error = "multiple messages in unary gRPC-Web response";
                return result;
            }
            result.message.assign(frame.begin(), frame.end());
            saw_data = true;
        } else {
            result.transport_error = "unsupported gRPC-Web frame flags";
            return result;
        }
    }

    if (!saw_status) {
        result.transport_error = "missing grpc-status";
        return result;
    }
    if (result.grpc_status == 0 && !saw_data) {
        result.transport_error = "missing unary response message";
        return result;
    }
    result.transport_ok = true;
    return result;
}

GrpcCallResult GrpcWebUnary(const std::string& host, uint16_t port, const std::string& path,
                            const std::vector<uint8_t>& request, int timeout_ms, const std::function<bool()>& interrupted)
{
    GrpcCallResult result;

    auto conn = TlsConnection::Connect(host, port, timeout_ms, result.transport_error, interrupted);
    if (!conn) return result;

    const std::vector<uint8_t> body = FrameMessage(request);

    std::string head;
    head += "POST " + path + " HTTP/1.1\r\n";
    head += "Host: " + host + "\r\n";
    head += "Content-Type: application/grpc-web+proto\r\n";
    head += "Accept: application/grpc-web+proto\r\n";
    head += "X-Grpc-Web: 1\r\n";
    head += "t"
            "e: trailers\r\n";
    head += "Content-Length: " + ToString(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";

    std::vector<uint8_t> out(head.begin(), head.end());
    out.insert(out.end(), body.begin(), body.end());
    if (!conn->WriteAll(out, result.transport_error)) return result;

    // Read the full response (Connection: close → read to EOF).
    std::vector<uint8_t> resp;
    std::vector<uint8_t> chunk(16384);
    for (;;) {
        int n = conn->Read(chunk, result.transport_error);
        if (n < 0) return result;
        if (n == 0) break;
        resp.insert(resp.end(), chunk.begin(), chunk.begin() + n);
        if (resp.size() > 32u * 1024 * 1024) { // guard
            result.transport_error = "response too large";
            return result;
        }
    }

    return ParseGrpcWebResponse(resp);
}

} // namespace platform::transport
