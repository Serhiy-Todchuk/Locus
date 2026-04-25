; All call expressions; @fn is the callee.
(call_expression
  function: (_) @fn) @call

; Calls to a specific identifier, e.g. `panic(...)`.
(call_expression
  function: (identifier) @fn
  (#eq? @fn "panic")) @call

; Selector calls, e.g. `fmt.Println(...)`; @method is the method name.
(call_expression
  function: (selector_expression field: (field_identifier) @method)) @call
