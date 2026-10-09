// Download queue: starts yt-dlp jobs, streams progress events, supports cancel.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dwn/process.hpp"

namespace dwn {

enum class Status { Queued, Running, Done, Failed, Cancelled };

struct DownloadItem {
    std::string id;
    std::string url;
    std::string title;
    std::string format_id;
    std::string out_dir;
    std::string filename;   // user-edited title, becomes the filename
    std::string player_clients;  // yt-dlp extractor args the fetch needed, if any
    std::string container;       // "" = as downloaded, "opus" = remux to Ogg Opus
    Status   status = Status::Queued;
    double   pct = 0.0;
    std::string speed;
    std::string eta;
    std::string error;
};

struct DownloadRequest {
    std::string url;
    std::string title;
    std::string format_id;   // must match ^[A-Za-z0-9+._-]+$
    std::string out_dir;
    std::string filename;    // optional user-edited title, used as the filename
    std::string cookies;     // Netscape cookie file body; may be empty
    // "youtube:player_client=..." from the fetch that listed the formats. The
    // download repeats it: without it yt-dlp can pick a different player client
    // and fail with "Requested format is not available".
    std::string player_clients;
    // Target container for an audio-only download: "" keeps whatever yt-dlp
    // wrote ("opus" remuxes it into an Ogg Opus file — a stream copy, not a
    // re-encode). Ignored for video jobs, where the video track decides.
    std::string container;
};

// Folds a user-edited title into a safe filename: path separators, traversal
// and shell/control characters are removed, ordinary titles are preserved.
std::string sanitize_filename(const std::string& value);

// Parses one line of `yt-dlp --newline` output.
// Returns true when the line carried progress information.
bool parse_progress_line(const std::string& line, DownloadItem& item);

// True when the line is a fatal yt-dlp error, with the reason in `error`.
bool parse_error_line(const std::string& line, std::string& error);

// Thread-safe queue. Events are emitted through the sink as JSON, ready to be
// written straight to the browser.
class DownloadQueue {
public:
    using EventFn = std::function<void(const std::string&)>;

    // Must be set before add(). Events: item, progress, done, error, state.
    void set_event_sink(EventFn fn) { sink_ = std::move(fn); }

    // Queues a download and returns its id. Starts immediately when a slot is
    // free. Returns "" if the request is invalid.
    std::string add(const DownloadRequest& req);

    // Cancels a job that has not started yet. Running jobs are left to finish:
    // killing yt-dlp mid-transfer would leave an unusable partial file.
    void cancel(const std::string& id);

    // Full queue state as JSON.
    std::string state_json() const;

    int max_concurrent = 2;

private:
    void emit(const std::string& json) const {
        if (sink_) sink_(json);
    }
    std::string item_json(const DownloadItem& item) const;
    // Runs one yt-dlp job. Named execute(), not run(), so it does not shadow
    // the dwn::run() process helper.
    void execute(const DownloadItem& item, const std::string& cookies);
    // Starts as many pending jobs as max_concurrent allows.
    void start_pending();

    mutable std::mutex        mutex_;
    std::vector<DownloadItem> items_;
    std::deque<std::string>   pending_;   // ids waiting for a slot
    std::map<std::string, CancellableProcess*> running_;  // id → live process
    std::map<std::string, std::string> cookies_;  // id → Netscape cookie body
    std::atomic<int>           next_id_{0};
    EventFn                    sink_;
};

}  // namespace dwn