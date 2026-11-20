# One dense layer: y = relu(x @ w + b). Fusion turns 3 kernels into 1.
input  x : [4, 8]
param  w : [8, 16]
param  b : [16]
h  = matmul(x, w)
h2 = add(h, b)          # broadcasting over the last dimension
y  = relu(h2)
output y
