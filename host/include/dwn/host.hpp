// JSON command dispatch: turns one request into one response, and may emit
// unsolicited events (download progress) in between.
#pragma once

#include <string>

namespace dwn {

// Sets the destination for download events (item / progress / state). Call once
// at startup, before dispatching any request.
void set_event_sink(void (*sink)(const std::string&));

// Handles a single raw request and returns the raw response to send back.
// Progress events for long-running commands are written directly with send()
// as they occur, not returned here.
std::string dispatch(const std::string& request);

// Synchronous fetch, used by the command line. dispatch() runs fetches on a
// thread and replies with {"type":"accepted"} first, which is meaningless for a
// one-shot command, so the CLI calls the fetcher directly and waits.
std::string fetch_now(const std::string& url, const std::string& cookies = "",
                      const std::string& browser = "");

// True when the last dispatched request asked the host to exit.
bool should_exit();

}  // namespace dwn