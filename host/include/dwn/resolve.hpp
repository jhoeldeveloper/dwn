// Locates the external tools the host depends on.
//
// Lookup order — first match wins, each candidate must be an executable file:
//
//   1. $YTDWL_TOOLS_DIR            explicit override, used by AppRun
//   2. <exe dir>/tools/            portable folder / AppImage layout
//   3. <exe dir>/                  tool sitting next to the binary
//   4. bare name                   resolved via PATH by the OS
//
// This is what lets one binary work in all three distribution modes without
// recompiling.
#pragma once

#include <string>

namespace dwn {

// Returns the path to use for `name` (e.g. "yt-dlp"). Falls back to `name`
// itself when nothing is found, letting the shell resolve it via PATH.
std::string resolve_tool(const std::string& name);

// True when the given path exists and has the owner-execute bit set.
bool is_executable(const std::string& path);

// Directory containing the running executable, or "" when it cannot be
// determined.
const std::string& exe_dir();

}  // namespace dwn