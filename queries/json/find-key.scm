; Every key / value pair, regardless of nesting depth.
(pair
  key:   (string) @key
  value: (_)      @value) @pair

; All pairs whose key is exactly "port" (e.g. across many service configs).
(pair
  key:   (string) @key
  value: (_)      @value
  (#eq? @key "\"port\"")) @pair

; All array literals -- useful for spotting list-shaped values.
(array) @array
