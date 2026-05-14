#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace locus {

// S5.B -- ANSI escape parser for the live terminal panel.
//
// Stream-friendly: feed it chunks via `consume()` and receive a vector of
// events. State (style, partial escape sequence) is preserved across calls,
// so an escape sequence split across two chunks parses correctly.
//
// Supports:
//   - SGR (`ESC [ ... m`): foreground colour (30-37, 90-97), background
//     colour (40-47, 100-107), bold, dim, reset.
//   - Erase-in-line (`ESC [ n K`): n=0 (to end of line, default), n=1 (to
//     start), n=2 (whole line).
//   - Erase-in-display (`ESC [ n J`): treated as "clear scrollback" for n=2
//     or n=3; n=0/1 are mapped to "erase to end of line" (closest scrollback
//     equivalent).
//
// Deliberately *not* supported:
//   - Cursor-position (`ESC [ ; H`, `ESC [ A`, etc.) -- a scrollback view
//     can't honour absolute cursor moves cleanly. Sequences are consumed
//     and silently dropped so they don't pollute the output.
//   - 256-colour / truecolour SGR (38;5;n / 38;2;r;g;b). Recognised and
//     dropped (i.e. fall back to default colour) rather than rendered.
//   - OSC sequences (`ESC ]`). Dropped through the next BEL or `ESC \`.
//
// Colours map to a 16-entry palette index (0-15) the renderer translates
// to wxColour. -1 means "default".

enum class AnsiEventKind {
    text,                // payload = run of plain text
    erase_to_eol,        // ESC [ 0 K   -- delete caret..EOL on current line
    erase_to_bol,        // ESC [ 1 K   -- delete BOL..caret on current line
    erase_line,          // ESC [ 2 K   -- delete whole current line
    erase_display,       // ESC [ 2 J / 3 J -- caller may clear the buffer
};

struct AnsiStyle {
    int8_t fg_index   = -1;   // 0..15, or -1 = default
    int8_t bg_index   = -1;
    bool   bold       = false;
    bool   dim        = false;

    bool operator==(const AnsiStyle& other) const = default;
};

struct AnsiEvent {
    AnsiEventKind kind = AnsiEventKind::text;
    std::string   text;     // populated only for `text` events
    AnsiStyle     style;    // active style for `text` events; ignored otherwise
};

class AnsiParser {
public:
    AnsiParser() = default;

    // Append a chunk of raw bytes; populate `out` with decoded events. Any
    // partial escape sequence at the chunk boundary is held in internal state
    // and resumed on the next call. Existing entries in `out` are not removed
    // -- events are appended.
    void consume(const char* data, std::size_t n, std::vector<AnsiEvent>& out);

    // Reset internal state (style + partial-sequence buffer). Caller typically
    // does this when clearing a terminal tab.
    void reset();

    // Current active style (read-only, for tests / introspection).
    const AnsiStyle& style() const { return style_; }

private:
    void flush_text(std::vector<AnsiEvent>& out);
    void apply_sgr(const std::vector<int>& params);
    void handle_csi(char final_byte, std::vector<AnsiEvent>& out);

    enum class State {
        ground,         // outside any escape sequence
        escape,         // just saw ESC (0x1B)
        csi,            // inside CSI: ESC [ ...
        osc,            // inside OSC: ESC ] ... (until BEL / ESC \)
    };

    State        state_ = State::ground;
    AnsiStyle    style_;            // current active style
    std::string  text_buf_;         // accumulating text run before flush
    std::string  csi_buf_;          // accumulating digits / ; inside CSI
    bool         osc_saw_escape_ = false;  // for OSC ESC \ termination
    // Pending \r that we haven't resolved yet -- if the next byte is \n we
    // treat the pair as a plain newline (Windows EOL); otherwise (incl. end
    // of chunk + no follow-up byte yet) we emit erase_line for the cmake /
    // ninja progress-overwrite case.
    bool         pending_cr_ = false;
};

} // namespace locus
