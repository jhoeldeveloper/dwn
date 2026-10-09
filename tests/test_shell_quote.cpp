// shell_quote is the only thing standing between a hostile URL or filename and
// arbitrary command execution, so test it adversarially.
#include "dwn/protocol.hpp"

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

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n       got:  %s\n       want: %s\n",
                    what.c_str(), got.c_str(), want.c_str());
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("shell_quote: basics\n");
    check_eq(dwn::shell_quote("yt-dlp"), "\"yt-dlp\"", "simple word is quoted");
    check_eq(dwn::shell_quote("/usr/bin/yt-dlp"), "\"/usr/bin/yt-dlp\"", "path is quoted");
    check_eq(dwn::shell_quote(""), "\"\"", "empty string stays quoted");

    std::printf("shell_quote: dangerous characters\n");
    // Every one of these must survive as data, never as syntax.
    check_eq(dwn::shell_quote("$(rm -rf /)"), "\"\\$(rm -rf /)\"", "command substitution");
    check_eq(dwn::shell_quote("`id`"), "\"\\`id\\`\"", "backtick substitution");
    check_eq(dwn::shell_quote("; rm -rf /"), "\"; rm -rf /\"", "command separator");
    check_eq(dwn::shell_quote("a\"b"), "\"a\\\"b\"", "embedded double quote");
    check_eq(dwn::shell_quote("a\\b"), "\"a\\\\b\"", "embedded backslash");
    check_eq(dwn::shell_quote("a&b"), "\"a&b\"", "background operator");
    check_eq(dwn::shell_quote("a|b"), "\"a|b\"", "pipe");
    check_eq(dwn::shell_quote("a>b"), "\"a>b\"", "redirect");
    check_eq(dwn::shell_quote("a\nb"), "\"a\nb\"", "newline stays inside quotes");
    check_eq(dwn::shell_quote("a$b"), "\"a\\$b\"", "variable expansion");
    check_eq(dwn::shell_quote("~/Videos"), "\"~/Videos\"", "tilde is not expanded");

    std::printf("shell_quote: real URLs\n");
    check_eq(dwn::shell_quote("https://youtube.com/watch?v=abc&t=30s"),
             "\"https://youtube.com/watch?v=abc&t=30s\"",
             "query string with & is neutralised");

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}