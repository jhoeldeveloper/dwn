#include "dwn/host.hpp"

#include "dwn/downloader.hpp"
#include "dwn/fetcher.hpp"
#include "dwn/protocol.hpp"
#include "dwn/resolve.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace dwn {

using nlohmann::json;

namespace {

std::atomic<bool> g_should_exit{false};

// Owned by main() for the life of the process, so detached worker threads
// always outlive it.
dwn::DownloadQueue g_queue;

json ok(const std::string& id) {
    return json{{"id", id}, {"type", "ok"}};
}

json error(const std::string& id, const std::string& message) {
    return json{{"id", id}, {"type", "error"}, {"error", message}};
}

// Reports what the host actually resolved for each external tool. Worth having
// visible: a "yt-dlp not found" bug is almost always this lookup going wrong
// rather than the tool being genuinely absent.
json ping(const std::string& id) {
    const char* home = std::getenv("HOME");
    return json{
        {"id", id},
        {"type", "ok"},
        {"host", "dwn/0.1.0"},
        {"home", home ? home : ""},
        {"tools",
         {{"yt-dlp", resolve_tool("yt-dlp")},
          {"ffmpeg", resolve_tool("ffmpeg")}}},
    };
}

// Handles "fetch": resolves the video's formats.
//
// The cookie material arrives as a Netscape cookie file body and is handed to
// the fetcher, which writes it to a private temp file for yt-dlp and removes
// it again immediately. It never reaches disk in the extension or in the
// response.
// Builds the "formats" reply shared by the protocol path and the CLI.
json formats_reply(const std::string& id, const dwn::MediaInfo& info) {
    json formats = json::array();
    for (const auto& f : info.formats) {
        formats.push_back({
            {"id", f.id},
            {"label", f.label},
            {"ext", f.ext},
            {"height", f.height},
            {"width", f.width},
            {"filesize", f.filesize},
            {"bitrate", f.bitrate},
            {"audio_only", f.audio_only},
            {"acodec", f.acodec},
        });
    }

    return json{
        {"id", id},
        {"type", "formats"},
        {"url", info.url},
        {"title", info.title},
        {"channel", info.channel},
        {"extractor", info.extractor},
        {"duration", info.duration},
        {"has_drm", info.has_drm},
        {"player_clients", info.player_clients},
        {"formats", formats},
    };
}

json handle_fetch(const std::string& id, const json& params) {
    const std::string url = params.value("url", "");
    if (url.empty()) {
        return error(id, "fetch requires a url");
    }

    // "browser" lets the CLI test the cookie path without the extension.
    const dwn::MediaInfo info =
        dwn::fetch(url, params.value("cookies", ""), params.value("browser", ""));

    if (info.failed) {
        return error(id, info.error);
    }
    return formats_reply(id, info);
}

// Handles "download". Returns immediately with an ack; the item, its progress
// events and the terminal state all arrive afterwards as unsolicited events.
json handle_download(const std::string& id, const json& params) {
    dwn::DownloadRequest req;
    req.url       = params.value("url", "");
    req.title     = params.value("title", "");
    req.format_id = params.value("format_id", "");
    req.out_dir   = params.value("out_dir", "");
    req.filename  = params.value("filename", "");
    req.cookies   = params.value("cookies", "");
    req.player_clients = params.value("player_clients", "");
    req.container     = params.value("container", "");

    if (req.url.empty())        return error(id, "download requires a url");
    if (req.format_id.empty())  return error(id, "download requires a format_id");
    if (req.out_dir.empty())    return error(id, "download requires an out_dir");

    const std::string download_id = g_queue.add(req);
    if (download_id.empty()) {
        return error(id, "invalid download request (format_id may only contain "
                         "letters, digits and + . _ -)");
    }
    return json{{"id", id}, {"type", "accepted"}, {"download_id", download_id}};
}

}  // namespace

std::string fetch_now(const std::string& url, const std::string& cookies,
                      const std::string& browser) {
    // The protocol path runs fetches on a thread and replies later, because a
    // slow fetch must not block the message loop. A one-shot CLI command has
    // nobody to reply to, so it calls the fetcher directly.
    if (url.empty()) return error("", "url is empty").dump();
    const dwn::MediaInfo info = dwn::fetch(url, cookies, browser);
    if (info.failed) return error("", info.error).dump();
    return formats_reply("", info).dump();
}

std::string dispatch(const std::string& request) {
    json req;
    try {
        req = json::parse(request);
    } catch (const std::exception& e) {
        return error("", std::string("malformed JSON: ") + e.what()).dump();
    }

    const std::string id     = req.value("id", "");
    const std::string cmd    = req.value("cmd", "");
    const json        params = req.value("params", json::object());

    log("cmd=" + (cmd.empty() ? std::string("<none>") : cmd));

    if (cmd == "ping") {
        return ping(id).dump();
    }
    if (cmd == "fetch") {
        // Fetching runs yt-dlp against the site and regularly takes 10-30s.
        // Doing it inline would block every other command — including queue
        // and cancel — for that whole time, so it runs on its own thread and
        // the reply is sent when it completes.
        const std::string request_id = id;
        const json        fetch_params = params;
        std::thread([request_id, fetch_params] {
            send(handle_fetch(request_id, fetch_params).dump());
        }).detach();
        return json{{"id", id}, {"type", "accepted"}}.dump();
    }
    if (cmd == "download") {
        return handle_download(id, params).dump();
    }
    if (cmd == "queue") {
        const json state = json::parse(g_queue.state_json());
        return json{{"id", id}, {"type", "state"},
                    {"items", state["items"]},
                    {"max_concurrent", g_queue.max_concurrent}}.dump();
    }
    if (cmd == "cancel") {
        g_queue.cancel(params.value("download_id", ""));
        return ok(id).dump();
    }
    if (cmd == "max_concurrent") {
        const int n = params.value("value", 2);
        if (n >= 1 && n <= 8) g_queue.max_concurrent = n;
        return json{{"id", id}, {"type", "ok"},
                    {"max_concurrent", g_queue.max_concurrent}}.dump();
    }
    if (cmd == "shutdown") {
        g_should_exit = true;
        return ok(id).dump();
    }

    // Implemented incrementally; see docs/protocol.md for the full command set.
    return error(id, "command not implemented: " + (cmd.empty() ? "<empty>" : cmd)).dump();
}

void set_event_sink(void (*sink)(const std::string&)) {
    g_queue.set_event_sink([sink](const std::string& json) { sink(json); });
}

bool should_exit() { return g_should_exit.load(); }

}  // namespace dwn