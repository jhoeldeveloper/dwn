// Parsing yt-dlp --newline output. These strings are copied verbatim from real
// yt-dlp runs, because the format varies by version and codec.
#include "dwn/downloader.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
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

void check_near(double got, double want, const std::string& what) {
    if (got > want - 0.01 && got < want + 0.01) {
        std::printf("  ok   %s (%.1f)\n", what.c_str(), got);
    } else {
        std::printf("  FAIL %s: got %.2f want %.2f\n", what.c_str(), got, want);
        ++g_failures;
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n       got:  '%s'\n       want: '%s'\n",
                    what.c_str(), got.c_str(), want.c_str());
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("downloader: progress lines\n");
    {
        dwn::DownloadItem item;
        const bool parsed = dwn::parse_progress_line(
            "[download]  23.5% of   85.32MiB at    2.50MiB/s ETA 00:30", item);
        check(parsed, "typical progress line parses");
        check_near(item.pct, 23.5, "percentage");
        check_eq(item.speed, "2.50MiB/s", "speed");
        check_eq(item.eta, "00:30", "eta");
    }
    {
        dwn::DownloadItem item;
        check(dwn::parse_progress_line("[download]   0.0% of 3.00MiB at   10.00KiB/s ETA 05:00", item),
              "0.0% parses");
        check_near(item.pct, 0.0, "zero percent");
    }
    {
        dwn::DownloadItem item;
        check(dwn::parse_progress_line("[download] 100% of   85.32MiB in    00:34", item),
              "completion line parses");
        check_near(item.pct, 100.0, "one hundred percent");
        // The completion line has no "at .../s", so the previous speed must
        // not be invented.
        check_eq(item.speed, "", "no speed invented on the completion line");
    }
    {
        dwn::DownloadItem item;
        check(dwn::parse_progress_line(
            "[download]  45.0% of 1.21GiB at  700.00KiB/s ETA 28:12", item),
              "GiB/MiB-scaled line parses");
        check_near(item.pct, 45.0, "percentage with GiB");
        check_eq(item.speed, "700.00KiB/s", "KiB/s speed");
    }

    std::printf("downloader: non-progress lines are ignored\n");
    for (const char* line : {
             "[youtube] abc123: Downloading webpage",
             "[info] Available formats for abc123:",
             "Destination: Big Buck Bunny.mp4",
             "",
             "[download] Destination: x.f137.mp4",
             "[Merger] Merging formats into \"x.mp4\"",
             "Deleting original file x.f137.mp4",
         }) {
        dwn::DownloadItem item;
        check(!dwn::parse_progress_line(line, item),
              std::string("ignored: ") + (line[0] ? line : "<empty>"));
    }

    std::printf("downloader: error lines\n");
    {
        std::string err;
        check(dwn::parse_error_line(
            "ERROR: [youtube] abc: Video unavailable. This video is not available", err),
            "error line detected");
        check_eq(err, "[youtube] abc: Video unavailable. This video is not available",
                 "message after ERROR: is kept");
    }
    {
        std::string err;
        check(dwn::parse_error_line("ERROR:   No space left on device", err),
              "leading colon and spaces are trimmed");
        check_eq(err, "No space left on device", "message is trimmed");
    }
    {
        std::string err;
        check(!dwn::parse_error_line("[download] 50% of 10MiB", err),
              "progress lines are not errors");
    }

    std::printf("downloader: request validation\n");
    {
        dwn::DownloadQueue queue;
        queue.max_concurrent = 1;

        dwn::DownloadRequest bad_url;
        bad_url.url = "";
        bad_url.format_id = "137";
        check(queue.add(bad_url) == "", "empty url is refused");

        dwn::DownloadRequest bad_format;
        bad_format.url = "https://example.test";
        bad_format.format_id = "137; rm -rf /";
        check(queue.add(bad_format) == "", "format_id with shell metacharacters is refused");

        dwn::DownloadRequest bad_format2;
        bad_format2.url = "https://example.test";
        bad_format2.format_id = "";
        check(queue.add(bad_format2) == "", "empty format_id is refused");
    }

    std::printf("downloader: filename handling\n");
    {
        // The title is user-editable and becomes a filename, so path
        // separators and traversal must not survive.
        const std::string clean = dwn::sanitize_filename("a/b/../c: d?e.mp4");
        // Traversal only matters if the result is a whole path component:
        // ".. " inside a name cannot escape the output directory.
        check(clean.find('/') == std::string::npos, "path separators are removed");
        check(clean.find(':') == std::string::npos, "colons are removed");

        // A name that IS ".." would mean the parent directory, so it must not
        // survive as a filename.
        check(dwn::sanitize_filename("..").empty(), "a bare '..' is rejected");
        check(dwn::sanitize_filename(".").empty(), "a bare '.' is rejected");

        const std::string empty = dwn::sanitize_filename("///");
        check(empty.empty(), "a name with nothing usable becomes empty");

        const std::string kept = dwn::sanitize_filename("Rick Astley - Never Gonna Give You Up (Official)");
        check(kept == "Rick Astley - Never Gonna Give You Up (Official)",
              "ordinary titles are preserved");

        const std::string long_name(400, 'a');
        check(dwn::sanitize_filename(long_name).size() <= 150,
              "very long names are truncated");
    }

    std::printf("downloader: merged formats\n");
    {
        // Picking a video and an audio track sends "137+140", which yt-dlp
        // merges with ffmpeg. Both characters must survive validation.
        //
        // Deliberately leaked: add() spawns a detached worker that references
        // the queue, which is safe in the app because the queue lives for the
        // whole process, but would dangle if a stack queue were destroyed here.
        static auto* queue = new dwn::DownloadQueue();
        queue->max_concurrent = 1;
        dwn::DownloadRequest merge;
        merge.url = "https://example.test";
        merge.format_id = "137+140";
        merge.out_dir = "/tmp";
        const std::string id = queue->add(merge);
        check(!id.empty(), "a merged format id like 137+140 is accepted");
    }

    std::printf("downloader: queue ordering metadata\n");
    {
        // The host emits the queue oldest-first; the panel sorts newest-first
        // using created_at. Two items added back to back must therefore carry
        // non-decreasing timestamps in insertion order.
        //
        // Deliberately leaked: add() spawns a detached worker that references
        // the queue (see the merged-format test above).
        static auto* queue = new dwn::DownloadQueue();
        queue->max_concurrent = 1;   // the second job stays queued
        std::string first_id;
        std::string second_id;
        for (int i = 0; i < 2; ++i) {
            dwn::DownloadRequest req;
            req.url = "https://example.test";
            req.format_id = "137";
            req.out_dir = "/tmp";
            const std::string id = queue->add(req);
            if (i == 0) first_id = id; else second_id = id;
        }

        const auto state = nlohmann::json::parse(queue->state_json());
        check(state["items"].is_array() && state["items"].size() >= 2,
              "state_json lists the queued items");

        long long previous = -1;
        bool stamped = true;
        bool ascending = true;
        std::string expected_order[] = {first_id, second_id};
        int position = 0;
        for (const auto& item : state["items"]) {
            const long long created = item.value("created_at", 0LL);
            if (created <= 0) stamped = false;
            if (created < previous) ascending = false;
            previous = created;
            if (position < 2 && item.value("download_id", "") == expected_order[position]) {
                ++position;
            }
        }
        check(stamped, "every item carries a non-zero created_at");
        check(ascending, "created_at never goes backwards in host order");
        check(position == 2, "items keep insertion order (the UI sorts them)");
    }

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}