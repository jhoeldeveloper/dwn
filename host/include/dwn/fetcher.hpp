// Fetches video metadata and the available formats via `yt-dlp --dump-json`.
#pragma once

#include <string>
#include <vector>

namespace dwn {

struct FormatInfo {
    std::string id;          // yt-dlp format_id, e.g. "137"
    std::string ext;         // "mp4", "webm", "m4a"
    int         height = 0;  // 0 for audio-only
    int         width  = 0;
    long long   filesize = 0;  // 0 when yt-dlp did not report one
    double      bitrate = 0;   // kbps, 0 when unknown
    bool        audio_only = false;
    std::string acodec;      // raw codec tag from yt-dlp, e.g. "opus", "mp4a.40.2"

    bool        has_video = false;
    bool        has_audio = false;

    // Human-readable summary for the UI, e.g. "1080p mp4 · 48 MB".
    std::string label;
};

struct MediaInfo {
    std::string url;
    std::string title;
    std::string channel;
    std::string extractor;
    int         duration = 0;
    bool        has_drm  = false;
    std::vector<FormatInfo> formats;

    bool        failed = false;
    std::string error;

    // The `youtube:player_client=...` value the successful answer came from,
    // empty when the default was fine. A download must repeat it, otherwise
    // yt-dlp may pick a different client on the second run and reject the
    // format id with "Requested format is not available".
    std::string player_clients;
};

// Fetches metadata for `url`.
//
// cookies_txt is Netscape-format cookie data supplied by the extension (see
// docs/protocol.md); pass "" to fetch anonymously. When cookies are supplied
// they are written to a private temp file, passed to yt-dlp, and deleted before
// returning.
//
// cookie_browser names a browser to read cookies from directly
// ("firefox", "brave", ...). It is used by the CLI for testing; the extension
// always supplies cookie_txt instead.
//
// Never throws: failures come back as MediaInfo::failed with ::error set.
MediaInfo fetch(const std::string& url, const std::string& cookies_txt,
                const std::string& cookie_browser = "");

// Exposed for testing: parses one yt-dlp JSON document into MediaInfo.
MediaInfo parse_metadata(const std::string& json, const std::string& url);

}  // namespace dwn