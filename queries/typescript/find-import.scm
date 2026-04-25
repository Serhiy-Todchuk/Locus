; `import { foo } from "bar"` -- @source is the module string.
(import_statement
  source: (string (string_fragment) @source)) @import

; Imports from a specific module, e.g. anything from "react".
(import_statement
  source: (string (string_fragment) @source)
  (#eq? @source "react")) @react_import
