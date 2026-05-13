; Top-level / nested function declarations.
(function_declaration
  (simple_identifier) @name) @fn

; Class declarations -- also covers Kotlin's `interface` keyword
; (fwcd's grammar uses class_declaration for both shapes).
(class_declaration
  (type_identifier) @name) @class

; Object declarations (`object Foo { ... }` -- singletons in Kotlin).
(object_declaration
  (type_identifier) @name) @object

; Property declarations (`val x`, `var y`).
(property_declaration) @prop

; Call expressions.
(call_expression) @call
