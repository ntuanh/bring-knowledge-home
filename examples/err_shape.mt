# Error example: the inner dimensions of the matmul do not match.
input  x : [4, 8]
param  w : [7, 16]
h = matmul(x, w)
y = relu(h)
output y
