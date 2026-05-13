; All function definitions; covers both `function foo() { ... }` and
; `foo() { ... }` (tree-sitter-bash collapses them into one node type).
(function_definition
  name: (word) @name) @fn

; Calls to `eval` -- a frequent footgun in shell scripts.
(command
  name: (command_name) @cmd
  (#eq? @cmd "eval")) @call

; Variable assignment shapes (FOO=bar at command-or-script level).
(variable_assignment
  name: (variable_name) @var
  value: (_)?           @value) @assign
