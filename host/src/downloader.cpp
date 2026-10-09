#include "dwn/downloader.hpp"

#include "dwn/process.hpp"
#include "dwn/protocol.hpp"
#include "dwn/resolve.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <regex>
#include <thread>

namespace dwn {

using nlohmann::json;

namespace {

// Folds a user-editable title into a safe filename: path separators, traversal
// and characters a filesystem dislikes become underscores. The result is also
// shell-quoted by the caller, so this is about the name, not injection.
std::string sanitize_filename_impl(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.'
            || c == '(' || c == ')' || c == '&' || c == '\'' || c == ',') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    // Trim, and collapse runs of whitespace.
    const auto start = out.find_first_not_of(" ._");
    const auto end = out.find_last_not_of(" ._");
    if (start == std::string::npos) return "";
    out = out.substr(start, end - start + 1);
    if (out.size() > 150) out.resize(150);
    return out;
}

// format_id is the one value that is validated rather than merely quoted,
// because it selects what yt-dlp resolves and must never be arbitrary text.
bool valid_format_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '+' || c == '.' || c == '_' || c == '-';
    });
}

// player_clients arrives from the browser, so it is validated like a format id:
// only "youtube:player_client=<names>" is accepted, never arbitrary arguments.
bool valid_player_clients(const std::string& value) {
    static const std::string prefix = "youtube:player_client=";
    if (value.empty()) return true;
    if (value.rfind(prefix, 0) != 0) return false;
    const std::string names = value.substr(prefix.size());
    if (names.empty() || names.size() > 200) return false;
    return std::all_of(names.begin(), names.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == ',' || c == '+';
    });
}

}  // namespace

std::string sanitize_filename(const std::string& value) {
    return sanitize_filename_impl(value);
}

bool parse_progress_line(const std::string& line, DownloadItem& item) {
    //   [download]  23.5% of   85.32MiB at    2.50MiB/s ETA 00:30
    //   [download] 100% of   85.32MiB in    00:34
    static const std::regex pct_re(R"(\[download\]\s+(\d+(?:\.\d+)?)%)");
    static const std::regex speed_re(R"(\bat\s+([\d.]+[KMG]i?B/s))");
    static const std::regex eta_re(R"(\bETA\s+(\d{2}:\d{2}))");

    std::smatch m;
    if (!std::regex_search(line, m, pct_re)) return false;

    item.pct = std::stod(m[1].str());
    if (std::regex_search(line, m, speed_re)) item.speed = m[1].str();
    if (std::regex_search(line, m, eta_re))   item.eta   = m[1].str();
    return true;
}

bool parse_error_line(const std::string& line, std::string& error) {
    const auto pos = line.find("ERROR:");
    if (pos == std::string::npos) return false;

    std::string message = line.substr(pos + 6);
    const auto start = message.find_first_not_of(" :");
    if (start != std::string::npos) message = message.substr(start);
    const auto caused = message.find(" (caused by ");
    if (caused != std::string::npos) message = message.substr(0, caused);

    error = message.empty() ? line : message;
    return true;
}

// Derives a usable message from yt-dlp's stderr.
//
// The live callback only sees stdout (that is where progress lives), so errors
// are read from stderr here once the process has exited. Prefers the first
// "ERROR:" line, which carries the real cause, over the warnings before it.
std::string extract_error(const std::string& stderr_text) {
    std::string best;
    std::string last_line;

    size_t pos = 0, nl;
    while (pos < stderr_text.size() &&
           (nl = stderr_text.find('\n', pos)) != std::string::npos) {
        const std::string line = stderr_text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        last_line = line;
        std::string message;
        if (parse_error_line(line, message) && best.empty()) best = message;
    }

    if (!best.empty()) return best;
    if (!last_line.empty()) return last_line;
    return "yt-dlp failed";
}

namespace {
// container is also attacker-controlled: only the supported values are
// accepted, so it can never smuggle an ffmpeg option. Both are lossless
// repackagings — ffmpeg picks the muxer from the target extension (Ogg Opus
// for .opus, ADTS for .aac) and copies the packets untouched.
bool valid_container(const std::string& value) {
    return value.empty() || value == "opus" || value == "aac";
}

// Rewrites a finished audio file into another container. `-c copy` never
// decodes, so the audio is identical: the ADTS muxer only adds a 7-byte
// header per frame, and the Ogg muxer wraps the same packets. Returns the new
// path, or "" on failure — the caller keeps the original either way.
std::string remux_to(const std::string& source, const std::string& extension) {
    namespace fs = std::filesystem;
    if (source.size() < 6 || extension.empty()) return "";

    fs::path in(source);
    fs::path out = in;
    out.replace_extension("." + extension);
    // Already there (either it was that container to begin with, or the
    // download produced it): nothing to do, and nothing to delete.
    if (out == in) return source;

    const std::string cmd = shell_quote(resolve_tool("ffmpeg")) +
                            " -y -loglevel error -i " + shell_quote(source) +
                            " -vn -c copy " + shell_quote(out.string());
    const RunResult result = run(cmd);
    if (result.exit_code != 0) {
        remove_file(out.string());
        log("ffmpeg failed: " + extract_error(result.err));
        return "";
    }
    return out.string();
}

}  // namespace

// ── queue ────────────────────────────────────────────────────────────────

namespace {
// How often a progress event is forwarded. yt-dlp reports several times per
// second; four is faster than anyone can read and a fraction of the traffic.
const std::chrono::milliseconds kProgressInterval{250};
}  // namespace

std::string DownloadQueue::item_json(const DownloadItem& item) const {
    const char* status = "queued";
    switch (item.status) {
        case Status::Queued:    status = "queued";    break;
        case Status::Running:   status = "running";   break;
        case Status::Done:      status = "done";      break;
        case Status::Failed:    status = "failed";    break;
        case Status::Cancelled: status = "cancelled"; break;
    }
    return json{
        {"type", "item"},
        {"download_id", item.id},
        {"url", item.url},
        {"title", item.title},
        {"format_id", item.format_id},
        {"out_dir", item.out_dir},
        {"status", status},
        {"pct", item.pct},
        {"speed", item.speed},
        {"eta", item.eta},
        {"error", item.error},
    }.dump();
}

std::string DownloadQueue::add(const DownloadRequest& req_in) {
    DownloadRequest req = req_in;
    if (req.url.empty()) return "";
    if (!valid_format_id(req.format_id)) {
        log("refusing format_id: " + req.format_id);
        return "";
    }
    if (!valid_player_clients(req.player_clients)) {
        log("refusing player_clients: " + req.player_clients);
        req.player_clients.clear();
    }
    if (!valid_container(req.container)) {
        log("refusing container: " + req.container);
        req.container.clear();
    }

    DownloadItem item;
    item.id        = std::to_string(next_id_++);
    item.url       = req.url;
    item.title     = req.title;
    item.format_id = req.format_id;
    item.out_dir   = req.out_dir;
    item.filename  = req.filename;
    item.player_clients = req.player_clients;
    item.container   = req.container;

    const std::string id = item.id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(item);
        pending_.push_back(id);
        // Cookie bodies are held only for the life of the job and erased in
        // run() once yt-dlp has finished with them.
        if (!req.cookies.empty()) cookies_[id] = req.cookies;
    }

    // Queued state, then whatever start() manages to launch.
    emit(item_json(item));
    start_pending();
    return id;
}

void DownloadQueue::start_pending() {
    for (;;) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            int active = 0;
            for (const auto& i : items_) {
                if (i.status == Status::Running) ++active;
            }
            if (active >= max_concurrent || pending_.empty()) return;

            id = pending_.front();
            pending_.pop_front();
            for (auto& i : items_) {
                if (i.id == id) i.status = Status::Running;
            }
        }

        // The queue outlives every job (it lives for the whole process), so
        // detached threads never see a destroyed object.
        std::thread([this, id] {
            DownloadItem snapshot;
            std::string  cookies;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& i : items_) {
                    if (i.id == id) snapshot = i;
                }
                cookies = cookies_.count(id) ? cookies_.at(id) : std::string();
            }
            execute(snapshot, cookies);
            // A slot just freed up — start the next queued job.
            start_pending();
        }).detach();
    }
}

void DownloadQueue::cancel(const std::string& id) {
    CancellableProcess* process = nullptr;
    bool queued = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Still waiting for a slot: drop it before it ever starts.
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (*it == id) { pending_.erase(it); queued = true; break; }
        }

        if (!queued) {
            const auto it = running_.find(id);
            if (it != running_.end()) process = it->second;
        }

        if (queued || process) {
            for (auto& i : items_) {
                if (i.id != id) continue;
                i.status = Status::Cancelled;
                emit(item_json(i));
            }
        }
    }

    // Killed outside the lock: teardown can block, and holding the queue mutex
    // would stall every other download's progress updates.
    if (process) {
        log("cancelling running download " + id);
        process->cancel();
    }
}

std::string DownloadQueue::state_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json items = json::array();
    for (const auto& i : items_) items.push_back(json::parse(item_json(i)));
    return json{{"type", "state"}, {"items", items},
                {"max_concurrent", max_concurrent}}.dump();
}

void DownloadQueue::execute(const DownloadItem& item, const std::string& cookies) {
    std::string cookie_path;
    if (!cookies.empty()) {
        cookie_path = write_private_temp_file("dwn-cookies-", cookies);
    }

    std::string head = shell_quote(resolve_tool("yt-dlp")) +
                       " --newline --no-colors --no-playlist" +
                       " -f " + shell_quote(item.format_id);
    // "video+audio" means yt-dlp must merge two streams, which needs ffmpeg
    // and a single output container.
    if (item.format_id.find('+') != std::string::npos) {
        head += " --merge-output-format mp4";
    }
    // A user-edited title becomes the filename; otherwise keep yt-dlp's own.
    const std::string name = sanitize_filename(item.filename);
    head += " -o " + shell_quote(item.out_dir + "/" +
                                 (name.empty() ? std::string("%(title)s") : name) +
                                 ".%(ext)s");

    // A target container only makes sense for an audio-only job: with a video
    // track the container is already decided by the video, and neither Ogg nor
    // ADTS can hold one. Asking yt-dlp to print the final path means the remux
    // below does not have to guess what it called the file.
    //
    // --progress is not redundant: --print puts yt-dlp into print-only mode,
    // which silences every [download] line. Without it back, the queue shows a
    // dead bar for the whole file and only jumps to done at the end.
    const bool remux_wanted = !item.container.empty() &&
                              item.format_id.find('+') == std::string::npos;
    if (remux_wanted) head += " --print after_move:filepath --progress";

    // YouTube resolves a player client per run, and the answer changes what is
    // downloadable: with the browser's cookies yt-dlp prefers the web client,
    // which hands back only low formats, and a format id the fetch listed then
    // comes back as "Requested format is not available". Cookies also turn
    // ordinary videos into "sign in to confirm" errors when they expire.
    //
    // So the download tries a few combinations, cheapest first, and stops at
    // the first one that starts transferring. Every failure here happens during
    // format selection, before a byte is written, so retrying is free:
    //   1. the client the fetch used, with cookies (needed for private videos)
    //   2. the clients that still return the full ladder, with cookies
    //   3. the same clients without cookies
    const char* kFallbackClients =
        "youtube:player_client=default,tv_embedded,android_testsuite,mediaconnect";
    const std::string fetch_args =
        item.player_clients.empty()
            ? std::string()
            : " --extractor-args " + shell_quote(item.player_clients);
    const std::string fallback_args =
        " --extractor-args " + shell_quote(kFallbackClients);

    struct Attempt {
        std::string args;
        bool        with_cookies;
    };
    std::vector<Attempt> attempts;
    attempts.push_back({fetch_args, true});
    if (fetch_args != fallback_args) attempts.push_back({fallback_args, true});
    if (!cookie_path.empty())     attempts.push_back({fallback_args, false});

    RunResult result;
    std::string error;
    std::string stderr_text;
    bool got_bytes = false;
    std::string final_path;   // filled by --print after_move:filepath

    for (size_t attempt = 0; attempt < attempts.size(); attempt++) {
        const bool use_cookies = attempts[attempt].with_cookies && !cookie_path.empty();
        DownloadItem progress_state = item;  // mutable copy for parsing
        double last_pct = -1.0;
        auto last_emit = std::chrono::steady_clock::now() - kProgressInterval;
        error.clear();
        stderr_text.clear();
        got_bytes = false;
        final_path.clear();

        std::string cmd = head + attempts[attempt].args;
        if (use_cookies) cmd += " --cookies " + shell_quote(cookie_path);
        cmd += " -- " + shell_quote(item.url);
        log("yt-dlp: " + resolve_tool("yt-dlp"));
        log("download: " + cmd);

        CancellableProcess proc(cmd);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_[item.id] = &proc;
        }

        proc.start([&](const std::string& line) {
            std::string err;
            if (parse_error_line(line, err)) { error = err; return; }
            // The --print line is an absolute path and nothing else, so it is
            // recognised by shape: no progress or diagnostic line yt-dlp emits
            // starts with a slash. The path itself may contain spaces.
            if (remux_wanted && line.size() > 5 && line.front() == '/') {
                final_path = line;
                return;
            }
            if (!parse_progress_line(line, progress_state)) return;
            got_bytes = true;

            // yt-dlp emits progress many times per second. Forwarding all of it
            // floods the native port and the browser's message channel, which is
            // where progress updates go to die under load — so a few per second
            // is plenty, and a human cannot read more than that anyway. The last
            // update is always sent by the completion path below.
            const auto now = std::chrono::steady_clock::now();
            const bool finished = progress_state.pct >= 100.0;
            if (!finished && now - last_emit < kProgressInterval) return;
            last_emit = now;
            last_pct  = progress_state.pct;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& i : items_) {
                    if (i.id != item.id) continue;
                    i.pct   = progress_state.pct;
                    i.speed = progress_state.speed;
                    i.eta   = progress_state.eta;
                }
            }
            emit(json{{"type", "progress"}, {"download_id", item.id},
                      {"pct", progress_state.pct}, {"speed", progress_state.speed},
                      {"eta", progress_state.eta}}.dump());
        });

        result = proc.wait();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(item.id);
        }

        // Retry the selection failures only, and only while nothing was
        // fetched. yt-dlp reports these on stderr, not on the --newline stdout
        // stream, so both have to be consulted or the retry never fires.
        stderr_text = result.err;
        // yt-dlp says why in stderr; the queue item only carries one line of it,
        // so the rest goes to the host log where scripts/debug-host.sh keeps it.
        if (!stderr_text.empty()) {
            std::string tail = stderr_text;
            if (tail.size() > 800) tail = tail.substr(tail.size() - 800);
            log("yt-dlp stderr: " + tail);
        }
        const std::string failure =
            !error.empty() ? error : extract_error(stderr_text);
        // "Format is not available" is the client lottery; the rest are the
        // ways expired or region-locked cookies show up.
        const bool retryable =
            failure.find("format is not available") != std::string::npos ||
            failure.find("Sign in to confirm") != std::string::npos ||
            failure.find("confirm your age") != std::string::npos ||
            failure.find("This video is available to Premium") != std::string::npos ||
            failure.find("members-only") != std::string::npos ||
            failure.find("requested format is not available") != std::string::npos;
        const bool cancelled = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& i : items_) if (i.id == item.id) return i.status == Status::Cancelled;
            return false;
        }();
        if (!retryable || got_bytes || cancelled) break;
        if (attempt + 1 < attempts.size()) {
            log("retrying the download with a different player client/cookie combination");
        }
        // No event here: a partial "item" message would overwrite the queue
        // entry's fields. The retry happens during format selection, so there
        // is no progress to report anyway.
    }

    // The cookie file holds credentials: gone as soon as yt-dlp exits.
    remove_file(cookie_path);

    const bool succeeded = result.exit_code == 0 && error.empty();

    // Repackaging runs BEFORE the lock is taken: an ffmpeg pass on a long file
    // takes seconds, and holding the queue lock across it would freeze every
    // other command — including the poll the panel uses to show progress.
    if (succeeded && remux_wanted && !final_path.empty()) {
        const std::string path = remux_to(final_path, item.container);
        if (path.empty()) {
            // Not a failed download: the original file is still on disk and
            // still good, so the item stays done and the reason goes to the log.
            log("could not write a ." + item.container + " file; keeping " + final_path);
        } else if (path != final_path) {
            remove_file(final_path);
            log("wrote " + path);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cookies_.erase(item.id);
    for (auto& i : items_) {
        if (i.id != item.id) continue;

        // cancel() already marked it and reported the reason; the non-zero exit
        // code from being killed is not a failure to report.
        if (i.status == Status::Cancelled) {
            emit(item_json(i));
            return;
        }

        if (succeeded) {
            i.status = Status::Done;
            i.pct = 100.0;
            i.speed.clear();
            i.eta.clear();
        } else {
            i.status = Status::Failed;
            i.error  = !error.empty() ? error : extract_error(stderr_text);
            if (i.error.empty()) {
                i.error = "yt-dlp exited with code " + std::to_string(result.exit_code);
            }
        }
        emit(item_json(i));
        return;
    }
}

}  // namespace dwn