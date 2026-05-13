; Every block mapping pair (the common `key: value` shape).
(block_mapping_pair
  key:   (_) @key
  value: (_) @value) @pair

; Pairs whose key is exactly "image" (e.g. across many Compose / k8s files).
(block_mapping_pair
  key:   (flow_node) @key
  value: (_)         @value
  (#eq? @key "image")) @pair

; Any flow sequence / block sequence -- list-shaped values.
(block_sequence) @list
(flow_sequence) @flow_list
