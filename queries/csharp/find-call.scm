; All invocation expressions; @fn captures the callee.
(invocation_expression
  function: (_) @fn) @call

; Invocations by exact identifier, e.g. `Console.WriteLine(...)`.
; @method is just the method name.
(invocation_expression
  function: (member_access_expression
    name: (identifier) @method)
  (#eq? @method "WriteLine")) @call

; All member-access invocations -- @method is the method name only.
(invocation_expression
  function: (member_access_expression
    name: (identifier) @method)) @call
