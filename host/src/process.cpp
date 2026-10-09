#include "dwn/process.hpp"

#include "dwn/protocol.hpp"

#include <process.hpp>

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace dwn {

// TinyProcessLibrary delivers output in arbitrary chunks that rarely align with
// line boundaries, so buffer until a newline arrives before emitting a line.
class LineSplitter {
public:
    explicit LineSplitter(std::function<void(const std::string&)> sink)
        : sink_(std::move(sink)) {}

    void feed(const char* bytes, size_t n) {
        buffer_.append(bytes, n);
        size_t pos = 0;
        size_t nl;
        while ((nl = buffer_.find('\n', pos)) != std::string::npos) {
            emit(buffer_.substr(pos, nl - pos));
            pos = nl + 1;
        }
        buffer_.erase(0, pos);
    }

    // Emits any trailing partial line once the process has exited.
    void flush() {
        if (!buffer_.empty()) {
            emit(buffer_);
            buffer_.clear();
        }
    }

private:
    void emit(std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && sink_) sink_(line);
    }

    std::string                           buffer_;
    std::function<void(const std::string&)> sink_;
};


RunResult run(const std::string& command, std::function<void(const std::string&)> on_line) {
    CancellableProcess proc(command);
    proc.start(std::move(on_line));
    return proc.wait();
}

CancellableProcess::CancellableProcess(std::string command)
    : command_(std::move(command)),
      result_(std::make_shared<RunResult>()) {}

CancellableProcess::~CancellableProcess() = default;

void CancellableProcess::start(std::function<void(const std::string&)> on_line) {
    // The splitters are members so wait() can flush whatever partial line the
    // process left behind when it exited or was killed.
    out_sink_ = std::make_shared<LineSplitter>(
        [this, on_line](const std::string& line) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_->out += line;
                result_->out += '\n';
            }
            // Called outside the lock: this hands work to the JSON writer,
            // which may itself block on the browser pipe.
            if (on_line) on_line(line);
        });

    err_sink_ = std::make_shared<LineSplitter>([this](const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_->err += line;
        result_->err += '\n';
    });

    // "exec" matters for cancellation: without it the command runs as a child
    // of /bin/sh, and killing the shell would leave yt-dlp running as an
    // orphan. With it the shell is replaced by yt-dlp, so a single signal
    // reaches the process actually doing the work.
    process_ = std::make_shared<TinyProcessLib::Process>(
        "exec " + command_, "",
        [this](const char* bytes, size_t n) { out_sink_->feed(bytes, n); },
        [this](const char* bytes, size_t n) { err_sink_->feed(bytes, n); }
    );
}

RunResult CancellableProcess::wait() {
    if (!process_) return {};

    result_->exit_code = process_->get_exit_status();
    out_sink_->flush();
    err_sink_->flush();
    return *result_;
}

void CancellableProcess::cancel() {
    if (!process_) return;
    // force=true sends SIGKILL. yt-dlp blocked on a socket read does not get to
    // clean up its partial file on a polite SIGTERM, and that file is discarded
    // either way, so a hard stop is the honest outcome.
    process_->kill(/*force=*/true);
}

std::string write_private_temp_file(const std::string& prefix, const std::string& text) {
    std::string pattern = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');

    const int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        log("mkstemp failed for " + prefix);
        return "";
    }
    // The file holds cookies: owner read/write only, never group or world.
    ::fchmod(fd, 0600);

    const char* data = text.data();
    size_t      left = text.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, data, left);
        if (written <= 0) {
            log("short write to " + prefix + " temp file");
            break;
        }
        data += written;
        left -= static_cast<size_t>(written);
    }
    ::close(fd);
    return std::string(buf.data());
}

void remove_file(const std::string& path) {
    if (!path.empty()) ::remove(path.c_str());
}

}  // namespace dwn