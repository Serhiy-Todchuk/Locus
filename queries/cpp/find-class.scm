; Every class / struct definition.
(class_specifier name: (type_identifier) @name) @class
(struct_specifier name: (type_identifier) @name) @struct

; Classes that inherit from a specific base, e.g. `ITool`.
; Replace "ITool" to target a different base type.
(class_specifier
  name: (type_identifier) @name
  (base_class_clause
    (type_identifier) @base
    (#eq? @base "ITool"))) @derived_class
