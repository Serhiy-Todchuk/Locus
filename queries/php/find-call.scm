; Free-function calls.
(function_call_expression
  function: (name) @fn) @call

; Method calls on an object: `$obj->foo(...)`.
(member_call_expression
  name: (name) @method) @call

; Static method calls: `Foo::bar(...)`.
(scoped_call_expression
  scope: (name)? @cls
  name:  (name)  @method) @call
