; All class / struct / interface / enum declarations; @name is the type name.
(class_declaration
  name: (identifier) @name) @class

(struct_declaration
  name: (identifier) @name) @struct

(interface_declaration
  name: (identifier) @name) @interface

(enum_declaration
  name: (identifier) @name) @enum

; Classes that derive from a specific base, e.g. `: Exception`.
; Replace "Exception" to target a different base type.
(class_declaration
  name: (identifier) @name
  (base_list (identifier) @base)
  (#eq? @base "Exception")) @derived_class

; Methods inside any class / interface / struct.
(method_declaration
  name: (identifier) @name) @method
