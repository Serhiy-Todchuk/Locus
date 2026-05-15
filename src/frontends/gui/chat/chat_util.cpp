#include "chat_util.h"

namespace locus {

wxString chat_js_escape(const wxString& s)
{
    wxString out;
    out.reserve(s.length() + 16);
    for (auto ch : s) {
        switch (ch.GetValue()) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '<':  out += "\\x3C"; break;
        default:   out += ch;      break;
        }
    }
    return out;
}

} // namespace locus
