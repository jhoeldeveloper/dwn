// Entry point for the native messaging host.
//
// Chrome launches this and speaks the length-prefixed JSON protocol on stdin
// and stdout. The loop below reads a frame, dispatches it, and writes the
// response; long-running commands emit events with send() as they progress.
//
// A command line is also accepted, which is how the host gets debugged without
// a browser: `dwn-host --ping`, `dwn-host --fetch <url>`.

#include "dwn/host.hpp"
#include "dwn/protocol.hpp"
#include "dwn/downloader.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

// Reads plain JSON requests from stdin, one per line, and prints plain JSON
// responses. Unlike the browser path there is no length prefix and no binary
// framing, which makes the host testable with nothing but a shell:
//     echo '{"id":"1","cmd":"ping"}' | dwn-host --stdin-json
int run_stdin_json() {
    // Events (progress, item updates) are unsolicited: without a sink the host
    // works silently and this mode cannot show whether a download is running.
    dwn::set_event_sink([](const std::string& json) { std::cout << json << std::endl; });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::cout << dwn::dispatch(line) << std::endl;
        if (dwn::should_exit()) break;
    }
    return 0;
}

// Downloads through the real queue, outside the browser. This is the only way
// to reproduce a download failure without the extension in the picture:
//     dwn-host --download <url> --format 137+251 [--out <dir>]
//     cat --cookies.txt | dwn-host --download <url> -f 137 --cookies-stdin
int run_download_cli(int argc, char** argv) {
    const std::string url = argv[2];
    std::string format, out_dir = ".", cookies, cookie_file, container;
    bool cookies_stdin = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        // Accepts both "--name value" and "--name=value". Each option is read
        // exactly once: reading it twice consumes the following argument the
        // second time, which is how "--format 140 --out /tmp" became "-f --out".
        std::string value;
        const char* names[] = {"--format", "-f", "--out", "--cookies", "--container", nullptr};
        for (const char** n = names; *n && value.empty(); ++n) {
            const std::string name(*n);
            if (arg == name && i + 1 < argc) value = argv[++i];
            else if (arg.rfind(name + "=", 0) == 0) value = arg.substr(name.size() + 1);
        }
        if (value.empty()) {
            if (arg == "--cookies") cookies_stdin = true;   // file path is not read yet
            else if (arg == "--cookies-stdin") cookies_stdin = true;
            continue;
        }
        if (arg.rfind("--format", 0) == 0 || arg.rfind("-f", 0) == 0) format = value;
        else if (arg.rfind("--out", 0) == 0) out_dir = value;
        else if (arg.rfind("--cookies", 0) == 0) { cookie_file = value; }
        else if (arg.rfind("--container", 0) == 0) { container = value; }
    }
    if (format.empty()) {
        std::cerr << "error: --format <id> is required\n";
        return 2;
    }
    if (cookies_stdin) {
        std::getline(std::cin, cookies, '\0');
    }
    if (!cookie_file.empty()) {
        std::ifstream in(cookie_file);
        cookies.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
    }

    dwn::DownloadQueue queue;
    dwn::Status last_status = dwn::Status::Queued;
    queue.set_event_sink([&](const std::string& json) {
        std::cout << json << std::endl;
        if (json.find("\"status\":\"done\"") != std::string::npos) last_status = dwn::Status::Done;
        if (json.find("\"status\":\"failed\"") != std::string::npos) last_status = dwn::Status::Failed;
    });

    dwn::DownloadRequest req;
    req.url       = url;
    req.format_id = format;
    req.out_dir   = out_dir;
    req.filename  = "";
    req.cookies   = cookies;
    req.player_clients.clear();
    req.container   = container;

    const std::string id = queue.add(req);
    if (id.empty()) {
        std::cerr << "error: the request was refused\n";
        return 1;
    }
    // add() returns immediately; wait for the job to reach a terminal state.
    while (last_status == dwn::Status::Queued) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return last_status == dwn::Status::Done ? 0 : 1;
}

// Prints one JSON response to stdout, unframed, for human use.
int run_cli(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "";
    if (first == "--stdin-json") {
        return run_stdin_json();
    }
    if (first == "--ping") {
        std::cout << dwn::dispatch(R"({"id":"1","cmd":"ping"})") << std::endl;
        return 0;
    }
    if (first == "--download" && argc > 2) {
        return run_download_cli(argc, argv);
    }
    if (first == "--fetch" && argc > 2) {
        const std::string url = argv[2];
        std::string browser;
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg.rfind("--browser=", 0) == 0) browser = arg.substr(10);
            else if (arg == "--browser" && i + 1 < argc) browser = argv[++i];
        }
        // fetch_now, not dispatch: dispatch runs fetches on a thread and only
        // acknowledges the request, which would leave the CLI with nothing.
        std::cout << dwn::fetch_now(url, "", browser) << std::endl;
        return 0;
    }

    std::cout <<
            "usage: dwn-host [--ping | --fetch <url> [--browser <name>]"
            " | --download <url> --format <id> [--out <dir>] [--cookies-stdin]"
            " | --stdin-json]\n"
            "\n"
            "  --ping                 report the host version and resolved tool paths\n"
            "  --fetch <url>          list the formats yt-dlp finds for <url>\n"
            "  --browser <name>       read cookies from that browser (firefox, brave, chrome)\n"
            "  --stdin-json           read one JSON request per line, print JSON replies\n"
            "                          (debugging: no browser and no framing involved)\n"
            "\n"
            "With no arguments the host speaks the native messaging protocol on\n"
            "stdin/stdout, which is how the browser extension drives it.\n";
    return (first == "--help" || first == "-h") ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    // Chrome passes the calling extension's origin as argv[1] when it launches
    // the host (on Linux and macOS). Anything not starting with "--" is that,
    // not a CLI request — treating it as one made the host print usage and
    // exit, which the browser reports as "Error when communicating with the
    // native messaging host".
    if (argc > 1 && std::string(argv[1]).rfind("--", 0) == 0) {
        return run_cli(argc, argv);
    }

    // stdout is buffered and used for binary framing. Ensure logging never
    // interleaves with it: all diagnostics go to stderr, and stdout is only
    // ever touched by write_message().
    std::setvbuf(stdout, nullptr, _IOFBF, 8192);

    // Download events bypass dispatch() and are written straight to the pipe.
    dwn::set_event_sink([](const std::string& json) { dwn::send(json); });

    dwn::log("host started");

    std::string request;
    while (dwn::read_message(request)) {
        const std::string response = dwn::dispatch(request);
        if (!response.empty()) dwn::send(response);
        if (dwn::should_exit()) break;
    }

    dwn::log("host exiting");
    return 0;
}