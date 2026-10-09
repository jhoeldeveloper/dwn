// Wire framing for Chrome/Brave native messaging.
//
// Format:  [uint32 length, native-endian][UTF-8 JSON payload]
//
// The length prefix uses the host's native byte order, which is what the browser
// expects. x86, x86-64 and ARM64 are all little-endian; a big-endian host would
// need a byteswap here.
//
// CRITICAL: stdout carries the protocol and nothing else. Any diagnostic output
// must go to stderr, otherwise it corrupts the stream and the browser tears the
// host down mid-conversation. Use log() for that.
#pragma once

#include <cstdio>
#include <string>

namespace dwn {

// Upper bound on a single frame. Anything larger is treated as a protocol
// error rather than a legitimate message — this stops a corrupt length prefix
// from turning into a huge allocation.
constexpr size_t kMaxFrameBytes = 64u * 1024u * 1024u;

// Reads one framed message. Blocks until a complete frame has arrived.
// Returns false on EOF, a short read, or a length outside the allowed range.
bool read_message(std::FILE* in, std::string& out);

// Writes one framed message. The length prefix and payload are written as
// separate calls so they never interleave with anything else sharing the fd.
bool write_message(std::FILE* out, const std::string& json);

// Convenience wrappers over stdin/stdout.
bool read_message(std::string& out);
bool write_message(const std::string& json);

// Sends a pre-serialised JSON message.
void send(const std::string& json);

// Wraps a value for /bin/sh. Every path or user-supplied string that ends up in
// a command string must go through this: the host builds commands that are
// executed via `/bin/sh -c`, so an unquoted value is command injection.
std::string shell_quote(const std::string& value);

// Writes a diagnostic line to stderr. Never write logs to stdout.
void log(const std::string& line);

}  // namespace dwn