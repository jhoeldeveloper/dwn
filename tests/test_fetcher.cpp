// Parsing a real yt-dlp payload. The fixture in tests/data was captured from a
// live `yt-dlp -J` run and deliberately keeps the awkward cases: null
// filesize, null height, null acodec, and a video that only carries audio.
#include "dwn/fetcher.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::filesystem::path fixture(const std::string& name) {
    // The fixture lives next to the sources; allow both in-tree and build runs.
    const std::filesystem::path candidates[] = {
        std::filesystem::path(DWN_FIXTURE_DIR) / name,
        std::filesystem::path("../tests/data") / name,
    };
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) return c;
    }
    return {};
}

}  // namespace

int main() {
    std::printf("fetcher: real yt-dlp payload\n");

    const auto path = fixture("ytdlp_video.json");
    check(!path.empty(), "fixture is found");
    if (path.empty()) return 1;

    const dwn::MediaInfo info = dwn::parse_metadata(slurp(path), "https://example.test/watch");

    check(!info.failed, "payload parses without error");
    if (info.failed) {
        std::printf("       error: %s\n", info.error.c_str());
        return 1;
    }

    check(info.title.find("Big Buck Bunny") != std::string::npos,
          "title is extracted");
    check(info.channel == "Blender", "channel is extracted");
    check(info.duration == 635, "duration is extracted");
    check(info.extractor == "youtube", "extractor is extracted");

    check(info.formats.size() == 10, "all 10 fixture formats are kept");

    // Storyboards must never reach the UI.
    bool has_storyboard = false;
    for (const auto& f : info.formats) {
        if (f.id.rfind("sb", 0) == 0) has_storyboard = true;
    }
    check(!has_storyboard, "storyboard formats are filtered out");

    // Sorted highest resolution first, audio last.
    check(!info.formats.front().audio_only, "first format is video, not audio");
    check(info.formats.front().height >= info.formats.back().height,
          "video formats are ordered by descending height");
    int audio_start = static_cast<int>(info.formats.size());
    for (size_t i = 0; i < info.formats.size(); ++i) {
        if (info.formats[i].audio_only) { audio_start = static_cast<int>(i); break; }
    }
    check(audio_start > 0 && audio_start < static_cast<int>(info.formats.size()),
          "audio formats are grouped after video");

    // The fixture's video formats report no size at all.
    check(info.formats.front().filesize == 0, "unknown filesize becomes 0, not garbage");
    check(info.formats.front().label.find('?') == std::string::npos,
          "a format with no size does not claim to have one");

    // 1080p should be at the front and labelled sensibly.
    bool found_1080 = false;
    for (const auto& f : info.formats) {
        if (f.height == 1080) {
            found_1080 = true;
            check(f.label.rfind("1080p", 0) == 0, "1080p label starts with the height");
            check(!f.audio_only, "1080p entry is not marked audio-only");
        }
    }
    check(found_1080, "the 1080p format survives parsing");

    // Format 233 has vcodec "none" AND a null acodec — still audio-only.
    for (const auto& f : info.formats) {
        if (f.id == "233") {
            check(f.audio_only, "null acodec still parses as audio-only");
        }
    }

    // Format 139-drc reports an exact filesize.
    for (const auto& f : info.formats) {
        if (f.id == "139-drc") {
            check(f.filesize == 3871021, "exact filesize is used when present");
            check(f.label.find("MiB") != std::string::npos, "size is humanised");
        }
    }

    std::printf("fetcher: failure modes\n");

    const dwn::MediaInfo broken = dwn::parse_metadata("{not json", "u");
    check(broken.failed, "malformed JSON reports failure");
    check(!broken.error.empty(), "malformed JSON explains itself");

    const dwn::MediaInfo empty = dwn::parse_metadata("{}", "u");
    check(empty.failed, "a payload with no formats reports failure");

    const dwn::MediaInfo drm = dwn::parse_metadata(
        R"({"title":"x","_has_drm":true,"formats":[{"format_id":"1","vcodec":"h264","acodec":"aac","height":720}]})",
        "u");
    check(!drm.failed, "DRM formats are still parsed");
    check(drm.has_drm, "DRM flag is detected");

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}