#include <catch2/catch_test_macros.hpp>

#include "frontends/gui/ansi_parser.h"

#include <string>
#include <vector>

using namespace locus;

namespace {

// Concatenate text-event payloads into one string for easy assertions.
std::string join_text(const std::vector<AnsiEvent>& events)
{
    std::string out;
    for (const auto& e : events)
        if (e.kind == AnsiEventKind::text) out += e.text;
    return out;
}

// Find the style of the n-th text event (0-indexed).
AnsiStyle text_style_at(const std::vector<AnsiEvent>& events, std::size_t idx)
{
    std::size_t seen = 0;
    for (const auto& e : events) {
        if (e.kind != AnsiEventKind::text) continue;
        if (seen == idx) return e.style;
        ++seen;
    }
    return {};
}

std::size_t count_kind(const std::vector<AnsiEvent>& events, AnsiEventKind k)
{
    std::size_t n = 0;
    for (const auto& e : events) if (e.kind == k) ++n;
    return n;
}

void feed(AnsiParser& p, const std::string& s, std::vector<AnsiEvent>& out)
{
    p.consume(s.data(), s.size(), out);
}

} // namespace

TEST_CASE("ANSI parser: plain text passes through unchanged", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "hello world\n", events);
    REQUIRE(join_text(events) == "hello world\n");
    REQUIRE(count_kind(events, AnsiEventKind::text) == 1);
}

TEST_CASE("ANSI parser: SGR red foreground sets fg index 1", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[31merror\x1b[0m done", events);
    REQUIRE(join_text(events) == "error done");
    REQUIRE(text_style_at(events, 0).fg_index == 1);
    REQUIRE(text_style_at(events, 1).fg_index == -1);
}

TEST_CASE("ANSI parser: bright green is palette index 10", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[92mOK\x1b[0m", events);
    REQUIRE(text_style_at(events, 0).fg_index == 10);
}

TEST_CASE("ANSI parser: bold + colour combine", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[1;34mhi\x1b[0m", events);
    REQUIRE(text_style_at(events, 0).bold == true);
    REQUIRE(text_style_at(events, 0).fg_index == 4);
}

TEST_CASE("ANSI parser: erase-to-EOL emits the right event", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "abc\x1b[Kdef", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_to_eol) == 1);
    REQUIRE(join_text(events) == "abcdef");
}

TEST_CASE("ANSI parser: ESC[2J emits erase_display", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "junk\x1b[2Jfresh", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_display) == 1);
    REQUIRE(join_text(events) == "junkfresh");
}

TEST_CASE("ANSI parser: ESC[1K -> erase_to_bol", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "abc\x1b[1Kdef", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_to_bol) == 1);
}

TEST_CASE("ANSI parser: ESC[2K -> erase_line", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "abc\x1b[2Kdef", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_line) == 1);
}

TEST_CASE("ANSI parser: bare CR emits erase_line (cmake/ninja progress)",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "[ 10%] Build\r[ 20%] Build", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_line) == 1);
    // Text is "[ 10%] Build" then "[ 20%] Build" -- both runs reach the panel.
    REQUIRE(join_text(events).find("10%") != std::string::npos);
    REQUIRE(join_text(events).find("20%") != std::string::npos);
}

TEST_CASE("ANSI parser: CRLF collapses to one newline (no erase_line)",
          "[s5.b][ansi]")
{
    // Windows EOL convention. Lone \r still erases (test above), but \r\n is
    // a plain line ending -- emitting erase_line for it wipes the preceding
    // text from the terminal panel, which is exactly the bug a real Python
    // -u "TEST OUTPUT\r\n" run hit.
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "TEST OUTPUT\r\n", events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_line) == 0);
    REQUIRE(join_text(events) == "TEST OUTPUT\n");
}

TEST_CASE("ANSI parser: CRLF straddling a chunk boundary still collapses",
          "[s5.b][ansi]")
{
    // The \r lands at the end of chunk 1; the \n at the start of chunk 2.
    // pending_cr_ must survive across consume() calls.
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "TEST OUTPUT\r", events);
    feed(parser, "\nMORE",        events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_line) == 0);
    REQUIRE(join_text(events) == "TEST OUTPUT\nMORE");
}

TEST_CASE("ANSI parser: lone CR straddling a chunk boundary still erases",
          "[s5.b][ansi]")
{
    // \r at end of chunk, then a non-\n byte at the start of the next chunk.
    // The deferred erase_line should fire on the first byte of the new
    // chunk so the progress-overwrite case still works across boundaries.
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "[ 10%] Build\r", events);
    feed(parser, "[ 20%] Build",  events);
    REQUIRE(count_kind(events, AnsiEventKind::erase_line) == 1);
    REQUIRE(join_text(events).find("10%") != std::string::npos);
    REQUIRE(join_text(events).find("20%") != std::string::npos);
}

TEST_CASE("ANSI parser: cursor-position sequences are dropped silently",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "a\x1b[5;10Hb", events);
    REQUIRE(join_text(events) == "ab");
    REQUIRE(count_kind(events, AnsiEventKind::text) >= 1);
}

TEST_CASE("ANSI parser: 256-colour sequence consumed but not styled",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[38;5;208morange\x1b[0m", events);
    REQUIRE(join_text(events) == "orange");
    // 38;5;208 should not leak an unintended fg colour
    REQUIRE(text_style_at(events, 0).fg_index == -1);
}

TEST_CASE("ANSI parser: escape split across chunk boundary parses correctly",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    // Split mid-CSI: "ESC [ 3" / "1 m red"
    feed(parser, "\x1b[3", events);
    feed(parser, "1m red", events);
    REQUIRE(join_text(events) == " red");
    REQUIRE(text_style_at(events, 0).fg_index == 1);
}

TEST_CASE("ANSI parser: escape split between ESC and [ parses correctly",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "a\x1b", events);
    feed(parser, "[33myellow", events);
    REQUIRE(join_text(events) == "ayellow");
    REQUIRE(text_style_at(events, 1).fg_index == 3);
}

TEST_CASE("ANSI parser: OSC sequence (set title) is consumed", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    // OSC 0; some title BEL  ->  silently dropped
    feed(parser, "before\x1b]0;hello\x07" "after", events);
    REQUIRE(join_text(events) == "beforeafter");
}

TEST_CASE("ANSI parser: reset clears active style and partial state",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[31mred", events);
    REQUIRE(text_style_at(events, 0).fg_index == 1);
    parser.reset();
    events.clear();
    feed(parser, "plain", events);
    REQUIRE(text_style_at(events, 0).fg_index == -1);
}

TEST_CASE("ANSI parser: bare ESC[m equals ESC[0m (reset)", "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[31mred\x1b[mnormal", events);
    REQUIRE(text_style_at(events, 0).fg_index == 1);
    REQUIRE(text_style_at(events, 1).fg_index == -1);
}

TEST_CASE("ANSI parser: high-volume input (10k chunks) preserves order",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    std::string expected;
    for (int i = 0; i < 1000; ++i) {
        std::string chunk = "line " + std::to_string(i) + "\n";
        expected += chunk;
        feed(parser, chunk, events);
    }
    REQUIRE(join_text(events) == expected);
}

TEST_CASE("ANSI parser: background colour 44 (blue bg) is recorded",
          "[s5.b][ansi]")
{
    AnsiParser parser;
    std::vector<AnsiEvent> events;
    feed(parser, "\x1b[44mblue-bg\x1b[0m", events);
    REQUIRE(text_style_at(events, 0).bg_index == 4);
    REQUIRE(text_style_at(events, 1).bg_index == -1);
}
