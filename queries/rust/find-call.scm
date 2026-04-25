; All function calls -- @fn is the callee path.
(call_expression
  function: (_) @fn) @call

; Calls to a specific path identifier, e.g. `Vec::new`.
(call_expression
  function: (scoped_identifier
    path: (identifier) @path
    name: (identifier) @name)
  (#eq? @path "Vec")
  (#eq? @name "new")) @call

; Method calls (`obj.save()`); @method is just the method name.
(call_expression
  function: (field_expression field: (field_identifier) @method)) @call
