; All typedefs; @name is the new type name.
(type_definition
  declarator: (type_identifier) @name) @typedef

; Typedefs of struct types, e.g. `typedef struct { int x; } Point;`.
(type_definition
  type: (struct_specifier) @struct
  declarator: (type_identifier) @name) @struct_typedef

; Typedefs of function pointers, e.g. `typedef int (*cb_t)(void *);`.
(type_definition
  declarator: (function_declarator) @sig) @fn_ptr_typedef
