# Error example: a tensor is used before it is defined.
input  x : [4, 8]
y = add(x, bias)
output y
