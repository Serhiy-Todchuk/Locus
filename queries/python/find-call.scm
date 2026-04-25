; All call sites; @fn is the callee, @call is the whole expression.
(call
  function: (_) @fn) @call

; Calls to a specific name, e.g. `print(...)`.
(call
  function: (identifier) @fn
  (#eq? @fn "print")) @call

; Method calls, e.g. `obj.save(...)` -- @method is just the method name.
(call
  function: (attribute attribute: (identifier) @method)) @call
