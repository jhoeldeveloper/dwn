// Framing round-trips and the failure modes that would desynchronise the stream.
#include "dwn/protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

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

// A temp file used as a fake pipe for the framing tests.
class TempFile {
public:
    TempFile() {
        char pattern[] = "/tmp/dwn-test-XXXXXX";
        const int fd = ::mkstemp(pattern);
        path_ = pattern;
        fp_ = ::fdopen(fd, "w+b");
    }
    ~TempFile() {
        if (fp_) std::fclose(fp_);
        ::remove(path_.c_str());
    }
    std::FILE* get() const { return fp_; }

private:
    std::string path_;
    std::FILE*  fp_ = nullptr;
};

std::vector<unsigned char> frame(const std::string& payload) {
    const auto length = static_cast<uint32_t>(payload.size());
    std::vector<unsigned char> bytes;
    bytes.resize(sizeof(uint32_t) + payload.size());
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        bytes[i] = static_cast<unsigned char>((length >> (8 * i)) & 0xFF);
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        bytes[sizeof(uint32_t) + i] = static_cast<unsigned char>(payload[i]);
    }
    return bytes;
}

void test_round_trip() {
    TempFile file;
    const std::string payload = R"({"id":"1","cmd":"ping"})";
    const auto bytes = frame(payload);

    std::fwrite(bytes.data(), 1, bytes.size(), file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string received;
    check(dwn::read_message(file.get(), received), "reads a complete frame");
    check(received == payload, "payload survives the round trip");
}

void test_two_messages_back_to_back() {
    TempFile file;
    const std::string first  = R"({"id":"1"})";
    const std::string second = R"({"id":"2","cmd":"shutdown"})";
    const auto a = frame(first);
    const auto b = frame(second);

    std::fwrite(a.data(), 1, a.size(), file.get());
    std::fwrite(b.data(), 1, b.size(), file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string one, two;
    check(dwn::read_message(file.get(), one), "reads the first of two frames");
    check(dwn::read_message(file.get(), two), "reads the second of two frames");
    check(one == first && two == second, "frames stay in order");
}

void test_eof() {
    TempFile file;
    std::string out;
    check(!dwn::read_message(file.get(), out), "clean EOF returns false");
}

void test_truncated_payload() {
    TempFile file;
    // Announce 100 bytes, supply only 10.
    const uint32_t length = 100;
    std::fwrite(&length, sizeof(length), 1, file.get());
    std::fwrite("0123456789", 1, 10, file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string out;
    // Declared length never arrives, so this must not spin or crash; the
    // trailing-bytes path returns false instead.
    const bool ok = dwn::read_message(file.get(), out);
    check(!ok, "truncated frame is rejected");
}

void test_oversized_length() {
    TempFile file;
    const uint32_t length = static_cast<uint32_t>(dwn::kMaxFrameBytes + 1);
    std::fwrite(&length, sizeof(length), 1, file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string out;
    const bool ok = dwn::read_message(file.get(), out);
    check(!ok, "oversized length is refused");
}

void test_zero_length() {
    TempFile file;
    const uint32_t length = 0;
    std::fwrite(&length, sizeof(length), 1, file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string out;
    const bool ok = dwn::read_message(file.get(), out);
    check(!ok, "zero length is refused");
}

void test_embedded_newline() {
    // JSON payloads contain newlines; framing must not be confused by them.
    TempFile file;
    const std::string payload = "{\n  \"id\": \"1\"\n}\n";
    const auto bytes = frame(payload);
    std::fwrite(bytes.data(), 1, bytes.size(), file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string received;
    check(dwn::read_message(file.get(), received), "reads a payload with newlines");
    check(received == payload, "embedded newlines are preserved exactly");
}

void test_write_then_read() {
    // Verifies write_message produces exactly what read_message expects.
    TempFile file;
    const std::string payload = R"({"id":"9","type":"ok"})";
    check(dwn::write_message(file.get(), payload), "write_message succeeds");
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string received;
    check(dwn::read_message(file.get(), received), "round-trips through a real file");
    check(received == payload, "write/read agree on the format");
}

void test_unframed_json() {
    // Brave can deliver a plain JSON message with no length prefix. Treating
    // the first four bytes as a length must not kill the host.
    TempFile file;
    const std::string payload = R"({"id":"5","cmd":"ping"})";
    std::fwrite(payload.data(), 1, payload.size(), file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string received;
    check(dwn::read_message(file.get(), received), "reads an unframed JSON message");
    check(received == payload, "unframed payload survives intact");
}

void test_framed_and_unframed_mixed() {
    TempFile file;
    const std::string framed_msg = R"({"id":"6"})";
    const std::string plain_msg  = R"({"id":"7","cmd":"shutdown"})";
    const auto bytes = frame(framed_msg);

    std::fwrite(bytes.data(), 1, bytes.size(), file.get());
    std::fwrite(plain_msg.data(), 1, plain_msg.size(), file.get());
    std::fflush(file.get());
    std::fseek(file.get(), 0, SEEK_SET);

    std::string one, two;
    check(dwn::read_message(file.get(), one), "reads the framed message first");
    check(dwn::read_message(file.get(), two), "then the unframed message");
    check(one == framed_msg && two == plain_msg, "both framings decode correctly");
}

}  // namespace

int main() {
    std::printf("protocol: framing\n");
    test_round_trip();
    test_two_messages_back_to_back();
    test_write_then_read();
    test_embedded_newline();
    test_unframed_json();
    test_framed_and_unframed_mixed();

    std::printf("protocol: failure modes\n");
    test_eof();
    test_truncated_payload();
    test_oversized_length();
    test_zero_length();

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}