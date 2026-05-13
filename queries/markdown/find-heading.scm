; ATX-style headings (`# ...`, `## ...`, etc.).
(atx_heading) @heading

; Fenced code blocks -- inspect language hints via the info_string.
(fenced_code_block
  (info_string)? @lang) @code

; Block quotes (Markdown's `> ...` shape).
(block_quote) @quote
