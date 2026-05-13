; All function declarations.
(function_declaration
  name: (_) @name) @fn

; All class declarations (also covers struct / enum / actor / extension --
; alex-pinkus's grammar folds them into class_declaration with a
; declaration_kind field).
(class_declaration
  name: (_) @name) @class

; Protocol declarations (Swift's equivalent of interfaces).
(protocol_declaration
  name: (_) @name) @protocol

; Initialisers (constructors).
(init_declaration) @init

; Function calls -- callee captured as @fn.
(call_expression
  (simple_identifier) @fn) @call
