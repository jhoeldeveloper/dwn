// Tool lookup order: env override, then <exe>/tools, then PATH.
#include "dwn/resolve.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

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

// Creates a file with the owner-execute bit set.
void make_executable(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "#!/bin/sh\n";
    fs::permissions(path, fs::perms::owner_all);
}

}  // namespace

int main() {
    std::printf("resolve: executable detection\n");
    const fs::path dir = fs::temp_directory_path() / "dwn-resolve-test";
    fs::remove_all(dir);

    const fs::path plain = dir / "plain.txt";
    fs::create_directories(dir);
    { std::ofstream out(plain); out << "x"; }
    check(!dwn::is_executable(plain.string()), "a non-executable file is rejected");

    const fs::path exe = dir / "tool";
    make_executable(exe);
    check(dwn::is_executable(exe.string()), "an executable file is accepted");

    check(!dwn::is_executable((dir / "missing").string()), "a missing file is rejected");
    check(!dwn::is_executable(dir.string()), "a directory is not a tool");

    std::printf("resolve: env override wins\n");
    const fs::path tools = dir / "tools";
    make_executable(tools / "yt-dlp");
    ::setenv("YTDWL_TOOLS_DIR", tools.c_str(), 1);
    check(dwn::resolve_tool("yt-dlp") == (tools / "yt-dlp").string(),
          "YTDWL_TOOLS_DIR is honoured");

    std::printf("resolve: falls back to PATH\n");
    ::unsetenv("YTDWL_TOOLS_DIR");
    // Nothing bundled exists for this name, so the bare name is returned and
    // the shell resolves it via PATH.
    check(dwn::resolve_tool("definitely-not-installed-xyz") == "definitely-not-installed-xyz",
          "unknown tools fall through to PATH");

    // A real one, to confirm the fallback is the bare name even when the tool
    // genuinely exists on the system.
    check(dwn::resolve_tool("sh") == "sh", "system tools resolve by bare name");

    fs::remove_all(dir);

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}