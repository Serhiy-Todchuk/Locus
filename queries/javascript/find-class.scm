; All class declarations; @name is the class name.
(class_declaration
  name: (identifier) @name) @class

; Classes that extend a specific base, e.g. `class Foo extends Error`.
; Replace "Error" to target a different superclass.
(class_declaration
  name: (identifier) @name
  (class_heritage (identifier) @base)
  (#eq? @base "Error")) @derived_class

; Methods inside any class.
(method_definition
  name: (property_identifier) @name) @method
