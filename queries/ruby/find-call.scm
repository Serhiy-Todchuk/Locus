; All method calls, with the callee captured as @fn.
(call
  method: (identifier) @fn) @call

; Calls to a specific method by name, e.g. `puts(...)`.
(call
  method: (identifier) @fn
  (#eq? @fn "puts")) @call

; All `def` method definitions.
(method
  name: (_) @name) @def

; All `def self.x` singleton (class-level) methods.
(singleton_method
  name: (_) @name) @def
