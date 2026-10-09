#include "dwn/protocol.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>

#include <unistd.h>

namespace dwn {

namespace {

// Responses are written from the main loop and from worker threads (an async
// fetch replies when it finishes), so every write to the shared stdout needs
// this lock. A frame must never interleave with another.
std::mutex& write_mutex() {
    static std::mutex instance;
    return instance;
}

// Partial input is buffered across calls. Chrome normally sends a length
// prefix, but it must be read without buffering the FILE stream because
// fread() blocks until the full request is satisfied, whereas a message can
// arrive in pieces and must be returned as soon as it is complete.
std::map<std::FILE*, std::string>& buffers() {
    static std::map<std::FILE*, std::string> instance;
    return instance;
}
std::mutex& buffers_mutex() {
    static std::mutex instance;
    return instance;
}

// Pulls whatever bytes are available. Returns false on EOF or error.
bool pull(std::FILE* in, std::string& buffer) {
    char chunk[8192];
    const ssize_t n = ::read(::fileno(in), chunk, sizeof(chunk));
    if (n <= 0) return false;
    buffer.append(chunk, static_cast<size_t>(n));
    return true;
}

}  // namespace

bool read_message(std::FILE* in, std::string& out) {
    std::lock_guard<std::mutex> lock(buffers_mutex());
    std::string& buffer = buffers()[in];

    for (;;) {
        // Preferred path: a length-prefixed frame.
        if (buffer.size() >= sizeof(uint32_t)) {
            uint32_t length = 0;
            std::memcpy(&length, buffer.data(), sizeof(length));
            if (length >= 1 && length <= kMaxFrameBytes &&
                buffer.size() >= sizeof(uint32_t) + length) {
                out = buffer.substr(sizeof(uint32_t), length);
                buffer.erase(0, sizeof(uint32_t) + length);
                return true;
            }
        }

        // Fallback: the browser sent bare JSON with no prefix. This happens
        // when a client posts a plain object, and treating the first four
        // bytes as a length would reject the message and kill the host.
        if (!buffer.empty() && buffer.front() == '{') {
            try {
                // Assign the result: a bare (void) call still trips GCC's
                // -Wunused-result on this function.
                const nlohmann::json parsed = nlohmann::json::parse(buffer);
                out = buffer;
                buffer.clear();
                log(std::string("unframed message accepted (") +
                    std::to_string(parsed.size()) + " top-level keys)");
                return true;
            } catch (const nlohmann::json::parse_error&) {
                // Incomplete so far — read more and try again.
            }
        }

        if (!pull(in, buffer)) {
            if (buffer.empty()) return false;
            // Trailing data with no complete message: treat it as unframed.
            if (buffer.front() == '{') {
                out = buffer;
                buffer.clear();
                return true;
            }
            log("discarding unparseable input (" + std::to_string(buffer.size()) + " bytes)");
            buffer.clear();
            return false;
        }
    }
}

bool write_message(std::FILE* out, const std::string& json) {
    std::lock_guard<std::mutex> lock(write_mutex());
    const auto length = static_cast<uint32_t>(json.size());
    if (std::fwrite(&length, sizeof(length), 1, out) != 1) return false;
    std::fflush(out);
    if (length == 0) return true;
    return std::fwrite(json.data(), 1, length, out) == length &&
           std::fflush(out) == 0;
}

bool read_message(std::string& out) { return read_message(stdin, out); }

bool write_message(const std::string& json) { return write_message(stdout, json); }

void send(const std::string& json) {
    if (!write_message(json)) {
        log("write failed — the browser has probably gone away");
    }
}

std::string shell_quote(const std::string& value) {
    // Double quotes make everything literal except " \ ` and $, which are the
    // characters that still matter inside a double-quoted string.
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '`':  out += "\\`";  break;
            case '$':  out += "\\$";  break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

void log(const std::string& line) {
    std::cerr << "[dwn] " << line << std::endl;
}

}  // namespace dwn