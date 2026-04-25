; if-statements with an empty `{}` body. Common smell after a refactor.
(if_statement
  consequence: (compound_statement) @body
  (#match? @body "^\\{\\s*\\}$")) @empty_if
