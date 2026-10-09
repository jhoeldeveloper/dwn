#include "dwn/fetcher.hpp"

#include "dwn/process.hpp"
#include "dwn/protocol.hpp"
#include "dwn/resolve.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace dwn {

using nlohmann::json;

namespace {

// yt-dlp omits many keys or sets them to null depending on the site and
// whether a size is known. These helpers treat "missing" and "null" alike,
// which is the difference between working and throwing on real payloads.
std::string str_or(const json& j, const char* key, const std::string& fallback = "") {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_string()) return fallback;
    return it->get<std::string>();
}

int int_or(const json& j, const char* key, int fallback = 0) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_number_float()) return static_cast<int>(it->get<double>());
    return fallback;
}

double dbl_or(const json& j, const char* key, double fallback = 0.0) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_number()) return it->get<double>();
    return fallback;
}

bool bool_or(const json& j, const char* key, bool fallback = false) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    return fallback;
}

// filesize is frequently null; filesize_approx is the fallback. 0 = unknown.
long long size_or(const json& j) {
    const auto exact = j.find("filesize");
    if (exact != j.end() && exact->is_number()) return exact->get<long long>();
    const auto approx = j.find("filesize_approx");
    if (approx != j.end() && approx->is_number()) return approx->get<long long>();
    return 0;
}

// YouTube usually reports no filesize at all, so bitrate is the fallback that
    // makes the size column meaningful — and it keeps otherwise identical
    // entries (e.g. two "144p VP9 mp4") distinguishable.
std::string human_size(long long bytes);

std::string human_bitrate(double kbps) {
    if (kbps <= 0) return "?";
    char buf[64];
    if (kbps >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1f Mbps", kbps / 1000.0);
    else                std::snprintf(buf, sizeof(buf), "%.0f kbps", kbps);
    return buf;
}

std::string size_label(const FormatInfo& info) {
    if (info.filesize > 0) return human_size(info.filesize);
    if (info.bitrate > 0)  return human_bitrate(info.bitrate);
    return "?";
}

std::string human_size(long long bytes) {
    if (bytes <= 0) return "?";
    static const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s",
                  value, units[unit]);
    return buf;
}

bool is_storyboard(const json& f) {
    // Storyboards are thumbnail grids, not media, and must never be offered.
    return str_or(f, "protocol") == "mhtml" || str_or(f, "format_note") == "storyboard";
}

FormatInfo parse_format(const json& f) {
    FormatInfo info;
    info.id    = str_or(f, "format_id");
    info.ext   = str_or(f, "ext");
    info.height = int_or(f, "height");
    info.width  = int_or(f, "width");
    info.filesize = size_or(f);

    // "none" means the stream carries no such data. A null acodec on a
    // vcodec=="none" format still means it is audio-only.
    const std::string vcodec = str_or(f, "vcodec");
    const std::string acodec = str_or(f, "acodec");

    info.has_video = !(vcodec == "none" || info.height <= 0);
    info.has_audio = !(acodec == "none") || vcodec == "none";
    info.audio_only = (vcodec == "none");
    info.acodec    = acodec;
    info.bitrate = info.audio_only ? dbl_or(f, "abr") : dbl_or(f, "tbr");
    if (info.bitrate <= 0) info.bitrate = dbl_or(f, "abr");

    // Short, recognisable name for the video codec. YouTube offers VP9, AV1 and
    // H.264 at the same resolution, so without this several entries render as
    // the same label ("2160p mp4") and are indistinguishable in the UI.
    std::string codec_tag = str_or(f, "vcodec");
    if (codec_tag.rfind("avc1", 0) == 0)      codec_tag = "H.264";
    else if (codec_tag.rfind("vp09", 0) == 0 ||
             codec_tag.rfind("vp9", 0) == 0)   codec_tag = "VP9";
    else if (codec_tag.rfind("av01", 0) == 0)  codec_tag = "AV1";
    else if (codec_tag.empty())                codec_tag.clear();

    const int fps = int_or(f, "fps");

    if (info.audio_only) {
        info.label = str_or(f, "format_note", "audio");
        if (!info.ext.empty()) info.label += " " + info.ext;
        // Codec matters more here than it does for video: "medium webm" and
        // "medium m4a" are Opus and AAC, and they are not interchangeable —
        // Opus plays in more players than its reputation suggests, but AAC is
        // the one that plays everywhere.
        const std::string acodec_tag = str_or(f, "acodec");
        if (acodec_tag.rfind("opus", 0) == 0)        info.label += " (Opus)";
        else if (acodec_tag.rfind("mp4a", 0) == 0)    info.label += " (AAC)";
        else if (acodec_tag == "mp3")                 info.label += " (MP3)";
        else if (!acodec_tag.empty() && acodec_tag != "none")
                                                     info.label += " (" + acodec_tag + ")";
    } else {
        if (info.height > 0) info.label = std::to_string(info.height) + "p";
        else info.label = "video";
        // Only worth showing when it is not the usual 30.
        if (fps > 45) info.label += std::to_string(fps);
        if (!codec_tag.empty()) info.label += " " + codec_tag;
        if (!info.ext.empty()) info.label += " " + info.ext;
    }
    info.label += " · " + size_label(info);

    // Dynamic range still matters when it is not SDR.
    std::string tag = str_or(f, "dynamic_range");
    if (tag.empty() || tag == "SDR") {
        const std::string note = str_or(f, "format_note");
        if (note.find("Dolby Vision") != std::string::npos) tag = "DV";
        else if (note.find("HDR") != std::string::npos) tag = "HDR";
        else if (note.find("DV") != std::string::npos) tag = "DV";
    }
    if (!tag.empty() && tag != "SDR") {
        info.label += " · " + tag;
    }

    return info;
}

// Pulls the most useful line out of yt-dlp's stderr. Falling back to the last
// line matters: the first "ERROR:" is often a wrapper and the real cause
// ("No video could be found in this tweet") is what the user needs to see.
std::string first_useful_error(const std::string& stderr_text) {
    std::string first_error;
    std::string last_line;
    size_t pos = 0, nl;
    while ((nl = stderr_text.find('\n', pos)) != std::string::npos) {
        std::string line = stderr_text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        last_line = line;
        const auto colon = line.find("ERROR:");
        if (colon != std::string::npos && first_error.empty()) {
            first_error = line.substr(colon + 6);
        }
    }
    if (!first_error.empty()) {
        // Trim the leading ": " and any trailing " (caused by ...)".
        size_t start = first_error.find_first_not_of(" :");
        if (start != std::string::npos) first_error = first_error.substr(start);
        const auto caused = first_error.find(" (caused by ");
        if (caused != std::string::npos) first_error = first_error.substr(0, caused);
        return first_error;
    }
    return last_line;
}

}  // namespace

namespace {

// A YouTube answer with only a handful of formats means the player client that
// answered was degraded: it hands back the single progressive mp4 (format 18,
// 360p) instead of the 1080p/1440p/2160p ladder. This is the symptom of a
// client that needs a PO token, or of a throttled/regional response, and it is
// indistinguishable from "this video only has one format" — which never
// happens for real uploads.
bool looks_degraded(const MediaInfo& info) {
    if (info.failed || info.formats.size() != 1) return false;
    const FormatInfo& f = info.formats.front();
    return !f.audio_only && f.has_audio && f.ext == "mp4" && f.height <= 360;
}

// Clients that still return the full ladder when the default selection is
// degraded. They are only used for the second attempt, so a yt-dlp too old to
// know one of them costs nothing: that run fails and the first result stands.
const char* kFallbackClients =
    "youtube:player_client=default,tv_embedded,android_testsuite,mediaconnect";

MediaInfo run_fetch(const std::string& yt_dlp, const std::string& extra_args,
                    const std::string& cookie_arg, const std::string& url,
                    const std::string& cookie_path, std::string* error_out) {
    std::string cmd = shell_quote(yt_dlp) +
                      " --dump-json --no-download --no-playlist --no-warnings";
    cmd += extra_args;
    cmd += cookie_arg;
    cmd += " -- " + shell_quote(url);

    const RunResult result = run(cmd);

    // The cookie file holds credentials: remove it as soon as yt-dlp exits,
    // whatever the outcome.
    remove_file(cookie_path);

    if (result.exit_code != 0) {
        MediaInfo info;
        info.url = url;
        info.failed = true;
        info.error  = first_useful_error(result.err);
        if (info.error.empty()) {
            info.error = "yt-dlp exited with code " + std::to_string(result.exit_code);
        }
        if (error_out) *error_out = info.error;
        return info;
    }
    return parse_metadata(result.out, url);
}

}  // namespace

MediaInfo parse_metadata(const std::string& raw, const std::string& url) {
    MediaInfo info;
    info.url = url;

    json data;
    try {
        data = json::parse(raw);
    } catch (const std::exception& e) {
        info.failed = true;
        info.error  = "could not parse yt-dlp output: " + std::string(e.what());
        return info;
    }

    info.title    = str_or(data, "title", "Unknown");
    info.channel  = str_or(data, "channel", str_or(data, "uploader", "Unknown"));
    info.extractor = str_or(data, "extractor");
    info.duration = int_or(data, "duration");
    info.has_drm  = bool_or(data, "_has_drm");

    const auto formats = data.find("formats");
    if (formats != data.end() && formats->is_array()) {
        for (const auto& f : *formats) {
            if (!f.is_object() || is_storyboard(f)) continue;
            FormatInfo parsed = parse_format(f);
            if (parsed.id.empty()) continue;
            if (!parsed.has_video && !parsed.has_audio) continue;
            info.formats.push_back(parsed);
        }
    }

    // Highest resolution first, then loudest audio — matches what people want
    // at the top of the list.
    std::stable_sort(info.formats.begin(), info.formats.end(),
                     [](const FormatInfo& a, const FormatInfo& b) {
                         // Video before audio: `a` sorts first when `b` is audio.
                         if (a.audio_only != b.audio_only) return b.audio_only;
                         if (a.height != b.height) return a.height > b.height;
                         return a.bitrate > b.bitrate;
                     });

    if (info.formats.empty()) {
        info.failed = true;
        info.error  = info.has_drm
            ? "this media is DRM-protected and cannot be downloaded"
            : "no downloadable video or audio was found";
    }
    return info;
}

MediaInfo fetch(const std::string& url, const std::string& cookies_txt,
                const std::string& cookie_browser) {
    if (url.empty()) {
        MediaInfo info;
        info.failed = true;
        info.error  = "url is empty";
        return info;
    }

    std::string cookie_path;
    if (!cookies_txt.empty()) {
        cookie_path = write_private_temp_file("dwn-cookies-", cookies_txt);
        if (cookie_path.empty()) {
            MediaInfo info;
            info.failed = true;
            info.error  = "could not write the cookie file";
            return info;
        }
    }

    // Build the command. --no-playlist keeps the output to a single JSON
    // document even when the URL points at a playlist or channel.
    const std::string yt_dlp = resolve_tool("yt-dlp");
    std::string cookie_arg;
    if (!cookie_path.empty()) {
        cookie_arg = " --cookies " + shell_quote(cookie_path);
    } else if (!cookie_browser.empty()) {
        // CLI/testing path: let yt-dlp read the browser's own cookie jar.
        cookie_arg = " --cookies-from-browser " + shell_quote(cookie_browser);
    }

    MediaInfo info = run_fetch(yt_dlp, "", cookie_arg, url, cookie_path, nullptr);

    // One progressive format means the client yt-dlp picked was degraded. Ask
    // again with an explicit client list, which is what actually restores the
    // 1080p entries; keep whichever answer has more to offer.
    if (looks_degraded(info)) {
        const std::string retry_args =
            " --extractor-args " + shell_quote(kFallbackClients);
        std::string retry_error;
        const MediaInfo retry =
            run_fetch(yt_dlp, retry_args, cookie_arg, url, cookie_path, &retry_error);
        if (!retry.failed && retry.formats.size() > info.formats.size()) {
            info = retry;
            // The download has to repeat this, or yt-dlp may answer from a
            // different client and call the same format id unavailable.
            info.player_clients = kFallbackClients;
        }
    }

    return info;
}

}  // namespace dwn