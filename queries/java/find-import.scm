; All import declarations; @path is the dotted module path.
(import_declaration
  (scoped_identifier) @path) @import

; Wildcard imports, e.g. `import java.util.*;`.
(import_declaration
  (asterisk)) @wildcard_import
