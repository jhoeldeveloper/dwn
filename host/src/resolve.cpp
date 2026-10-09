#include "dwn/resolve.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dwn {

namespace {

namespace fs = std::filesystem;

}  // namespace

bool is_executable(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    const auto perms = fs::status(path, ec).permissions();
    if (ec) return false;
    return (perms & fs::perms::owner_exec) != fs::perms::none;
}

const std::string& exe_dir() {
    static const std::string dir = [] {
#ifndef _WIN32
        // /proc/self/exe resolves symlinks, so launching through a wrapper
        // script or a symlink still finds the real binary's directory.
        char    buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string();
        buf[n] = '\0';
        return fs::path(buf).parent_path().string();
#else
        return std::string();
#endif
    }();
    return dir;
}

std::string resolve_tool(const std::string& name) {
    if (const char* dir = std::getenv("YTDWL_TOOLS_DIR")) {
        if (dir[0] != '\0') {
            const fs::path candidate = fs::path(dir) / name;
            if (is_executable(candidate.string())) return candidate.string();
        }
    }

    const std::string& dir = exe_dir();
    if (!dir.empty()) {
        const fs::path tools = fs::path(dir) / "tools" / name;
        if (is_executable(tools.string())) return tools.string();

        const fs::path sibling = fs::path(dir) / name;
        if (is_executable(sibling.string())) return sibling.string();
    }

    return name;  // fall back to PATH
}

}  // namespace dwn