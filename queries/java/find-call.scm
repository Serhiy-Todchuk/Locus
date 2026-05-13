; All method invocations; @method is the method name.
(method_invocation
  name: (identifier) @method) @call

; Invocations of a specific method, e.g. `System.out.println(...)`.
; Replace "println" to target a different method name.
(method_invocation
  name: (identifier) @method
  (#eq? @method "println")) @call

; Calls on a specific receiver, e.g. `log.warn(...)`.
(method_invocation
  object: (identifier) @receiver
  name: (identifier) @method
  (#eq? @receiver "log")) @call
