; All call expressions; @fn captures the callee.
(call_expression
  function: (_) @fn) @call

; Calls by exact name, e.g. `setTimeout(...)`.
(call_expression
  function: (identifier) @fn
  (#eq? @fn "setTimeout")) @call

; Method calls -- captures just the method name (`obj.save()` -> @method = "save").
(call_expression
  function: (member_expression property: (property_identifier) @method)) @call
