; All `using` directives; @namespace is the dotted namespace path.
(using_directive
  (qualified_name) @namespace) @using

; Simple `using Foo;` -- single identifier rather than qualified name.
(using_directive
  (identifier) @namespace) @using_simple
