; `import x` and `import x.y.z`.
(import_statement
  name: (dotted_name) @module) @import

; `from x import y, z`.
(import_from_statement
  module_name: (dotted_name) @module
  name: (dotted_name) @symbol) @from_import
