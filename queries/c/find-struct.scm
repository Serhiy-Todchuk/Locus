; Named struct definitions; @name is the struct tag.
(struct_specifier
  name: (type_identifier) @name) @struct

; Named union definitions.
(union_specifier
  name: (type_identifier) @name) @union

; Named enum definitions.
(enum_specifier
  name: (type_identifier) @name) @enum
