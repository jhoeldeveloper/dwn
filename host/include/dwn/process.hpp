// Spawns external commands (yt-dlp, ffmpeg) and captures their output.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace TinyProcessLib {
class Process;
}

namespace dwn {

// Implementation detail of process.cpp, but it appears in the interface below.
class LineSplitter;

struct RunResult {
    int         exit_code = -1;
    std::string out;
    std::string err;
};

// Runs a command through /bin/sh, invoking on_line for each complete stdout
// line as it arrives. Used for live download progress.
// Blocks until the process exits.
RunResult run(const std::string& command,
              std::function<void(const std::string&)> on_line = {});

// A command that can be stopped from another thread.
//
// Cancelling a download means killing yt-dlp mid-transfer, which the blocking
// run() above cannot do. Callers keep the handle, and cancel() terminates the
// process; the caller then waits for the result as usual.
class CancellableProcess {
public:
    explicit CancellableProcess(std::string command);
    ~CancellableProcess();

    CancellableProcess(const CancellableProcess&) = delete;
    CancellableProcess& operator=(const CancellableProcess&) = delete;

    // Starts the process. on_line receives each stdout line as it arrives.
    void start(std::function<void(const std::string&)> on_line);

    // Asks the process to stop: SIGTERM first, then SIGKILL if it is still
    // alive a moment later. Safe to call from any thread, and safe to call
    // after the process has already exited.
    void cancel();

    // Blocks until the process exits and returns its output.
    RunResult wait();

private:
    std::string command_;
    std::shared_ptr<TinyProcessLib::Process> process_;
    std::shared_ptr<RunResult> result_;
    std::shared_ptr<LineSplitter> out_sink_;
    std::shared_ptr<LineSplitter> err_sink_;
    std::mutex mutex_;
};

// Writes text to a private temporary file (mode 0600) and returns its path.
// Used for cookies.txt, which holds credentials. Caller owns the file and must
// call remove_file(). Returns "" on failure.
std::string write_private_temp_file(const std::string& prefix,
                                    const std::string& text);

void remove_file(const std::string& path);

}  // namespace dwn