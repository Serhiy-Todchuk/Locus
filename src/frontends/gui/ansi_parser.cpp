#include "ansi_parser.h"

#include <charconv>

namespace locus {

namespace {

constexpr char k_esc = 0x1B;
constexpr char k_bel = 0x07;

// Parse the contents of `csi_buf_` (digits and ';' separators) into a list of
// ints. Empty parameter slots default to 0 (the SGR convention: `\x1b[m` is
// equivalent to `\x1b[0m`).
std::vector<int> parse_csi_params(const std::string& s)
{
    std::vector<int> params;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        int value = 0;
        if (end > pos) {
            auto* first = s.data() + pos;
            auto* last  = s.data() + end;
            auto [ptr, ec] = std::from_chars(first, last, value);
            (void)ptr;
            if (ec != std::errc{}) value = 0;
        }
        params.push_back(value);
        if (end == s.size()) break;
        pos = end + 1;
    }
    if (params.empty()) params.push_back(0);
    return params;
}

// Map an SGR colour code to a palette index 0-15.
// 30-37 / 40-47 -> 0-7;  90-97 / 100-107 -> 8-15.
int8_t sgr_to_palette(int code, int base_normal, int base_bright)
{
    if (code >= base_normal && code <= base_normal + 7)
        return static_cast<int8_t>(code - base_normal);
    if (code >= base_bright && code <= base_bright + 7)
        return static_cast<int8_t>(8 + code - base_bright);
    return -1;
}

} // namespace

void AnsiParser::reset()
{
    state_          = State::ground;
    style_          = AnsiStyle{};
    text_buf_.clear();
    csi_buf_.clear();
    osc_saw_escape_ = false;
    pending_cr_     = false;
}

void AnsiParser::flush_text(std::vector<AnsiEvent>& out)
{
    if (text_buf_.empty()) return;
    AnsiEvent e;
    e.kind  = AnsiEventKind::text;
    e.text  = std::move(text_buf_);
    e.style = style_;
    out.push_back(std::move(e));
    text_buf_.clear();
}

void AnsiParser::apply_sgr(const std::vector<int>& params)
{
    for (std::size_t i = 0; i < params.size(); ++i) {
        int p = params[i];
        if (p == 0) {
            style_ = AnsiStyle{};
        } else if (p == 1) {
            style_.bold = true;
        } else if (p == 2) {
            style_.dim = true;
        } else if (p == 22) {
            style_.bold = false; style_.dim = false;
        } else if (p == 39) {
            style_.fg_index = -1;
        } else if (p == 49) {
            style_.bg_index = -1;
        } else if (p == 38 || p == 48) {
            // Extended colour: 38;5;n (256) or 38;2;r;g;b (truecolour).
            // Consume + drop. The renderer falls back to the existing style.
            if (i + 1 < params.size() && params[i + 1] == 5) {
                i += 2; // skip mode + index
            } else if (i + 1 < params.size() && params[i + 1] == 2) {
                i += 4; // skip mode + r + g + b
            } else {
                i = params.size(); // malformed, bail
            }
        } else {
            int8_t fg = sgr_to_palette(p, 30, 90);
            int8_t bg = sgr_to_palette(p, 40, 100);
            if (fg >= 0) style_.fg_index = fg;
            else if (bg >= 0) style_.bg_index = bg;
            // Unknown codes are silently dropped.
        }
    }
}

void AnsiParser::handle_csi(char final_byte, std::vector<AnsiEvent>& out)
{
    auto params = parse_csi_params(csi_buf_);
    switch (final_byte) {
        case 'm': {
            flush_text(out);
            apply_sgr(params);
            break;
        }
        case 'K': {
            flush_text(out);
            AnsiEvent e;
            switch (params[0]) {
                case 0: e.kind = AnsiEventKind::erase_to_eol; break;
                case 1: e.kind = AnsiEventKind::erase_to_bol; break;
                case 2: e.kind = AnsiEventKind::erase_line;   break;
                default: return;
            }
            out.push_back(std::move(e));
            break;
        }
        case 'J': {
            flush_text(out);
            // n=0 (to end of display) and n=1 (to start) are approximated as
            // erase-to-EOL on the current line; only n=2 / n=3 clear the
            // whole scrollback view.
            AnsiEvent e;
            if (params[0] >= 2) e.kind = AnsiEventKind::erase_display;
            else                e.kind = AnsiEventKind::erase_to_eol;
            out.push_back(std::move(e));
            break;
        }
        default:
            // Other CSI finals (cursor moves H/A/B/C/D/G, scroll regions r,
            // SGR mouse h/l, etc.) are deliberately ignored -- a scrollback
            // view can't honour them coherently.
            break;
    }
    csi_buf_.clear();
}

void AnsiParser::consume(const char* data, std::size_t n, std::vector<AnsiEvent>& out)
{
    for (std::size_t i = 0; i < n; ++i) {
        char c = data[i];

        // Resolve any deferred \r BEFORE processing this byte. \r\n is the
        // normal Windows line ending and must produce a plain newline, not
        // an erase-line. Lone \r still gets the erase-line semantics for
        // cmake / ninja progress-overwrite. The deferral lets us straddle
        // chunk boundaries -- a \r at the end of one chunk and \n at the
        // start of the next still collapses to one newline.
        if (pending_cr_) {
            pending_cr_ = false;
            if (c == '\n') {
                text_buf_.push_back('\n');
                continue;
            }
            // Anything else: emit the deferred erase_line, then fall through
            // and process this byte normally.
            flush_text(out);
            AnsiEvent e; e.kind = AnsiEventKind::erase_line;
            out.push_back(std::move(e));
        }

        switch (state_) {
            case State::ground:
                if (c == k_esc) {
                    flush_text(out);
                    state_ = State::escape;
                } else if (c == '\r') {
                    // Defer: might be the start of a Windows \r\n pair.
                    pending_cr_ = true;
                } else {
                    text_buf_.push_back(c);
                }
                break;

            case State::escape:
                if (c == '[') {
                    state_ = State::csi;
                    csi_buf_.clear();
                } else if (c == ']') {
                    state_ = State::osc;
                    osc_saw_escape_ = false;
                } else {
                    // Unknown / single-byte escape -- drop the ESC, return
                    // to ground. Reprocess the current byte so e.g. "ESC A"
                    // doesn't swallow the A.
                    state_ = State::ground;
                    --i;
                }
                break;

            case State::csi: {
                // Parameter bytes 0x30-0x3F (digits, ';', ':') accumulate;
                // intermediate bytes 0x20-0x2F accumulate but rarely matter;
                // final byte 0x40-0x7E terminates the sequence.
                unsigned char uc = static_cast<unsigned char>(c);
                if (uc >= 0x30 && uc <= 0x3F) {
                    csi_buf_.push_back(c);
                } else if (uc >= 0x20 && uc <= 0x2F) {
                    // Intermediate. Ignore for now; common case is none.
                } else if (uc >= 0x40 && uc <= 0x7E) {
                    handle_csi(c, out);
                    state_ = State::ground;
                } else {
                    // Stray control byte inside CSI -- bail out, treat as text.
                    state_ = State::ground;
                    text_buf_.push_back(c);
                }
                break;
            }

            case State::osc:
                // Terminated by BEL or `ESC` then backslash.
                if (c == k_bel) {
                    state_ = State::ground;
                } else if (osc_saw_escape_) {
                    // Previous byte was ESC; this byte ends the OSC iff '\\'.
                    osc_saw_escape_ = false;
                    state_ = State::ground;
                } else if (c == k_esc) {
                    osc_saw_escape_ = true;
                }
                // Body bytes silently dropped.
                break;
        }
    }
    // Don't flush leftover text here -- if the next chunk starts inside an
    // escape sequence we'd otherwise emit a half-line. Callers can poll
    // `consume` again with zero bytes if they want a forced flush, or call
    // `flush_pending` (added below if needed). In practice the renderer
    // re-invokes consume on every chunk; trailing text is emitted on the
    // next call's first text byte via flush_text.
    if (state_ == State::ground)
        flush_text(out);
}

} // namespace locus
