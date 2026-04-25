; All call expressions, with the callee captured as @fn.
; Combine with `capture=fn` on the tool to get only the callee names.
(call_expression
  function: (_) @fn) @call

; Calls to a specific function by name, e.g. `malloc(...)`.
; Replace "malloc" to target a different identifier.
(call_expression
  function: (identifier) @fn
  (#eq? @fn "malloc")) @call
