; All class / interface / enum declarations; @name is the type name.
(class_declaration
  name: (identifier) @name) @class

(interface_declaration
  name: (identifier) @name) @interface

(enum_declaration
  name: (identifier) @name) @enum

; Classes that extend a specific superclass, e.g. `extends Exception`.
; Replace "Exception" to target a different base type.
(class_declaration
  name: (identifier) @name
  (superclass (type_identifier) @base)
  (#eq? @base "Exception")) @derived_class

; Methods inside any class / interface.
(method_declaration
  name: (identifier) @name) @method
