#pragma once

#include <wx/string.h>

namespace locus {

// JS string-literal escaper used by ChatPanel and its collaborators when
// composing RunScript fragments. Escapes \, ', \n, \r, \t and `<` so the
// result is safe to wrap in single quotes and so the body cannot prematurely
// close an enclosing `<script>` tag if echoed into HTML.
wxString chat_js_escape(const wxString& s);

} // namespace locus
