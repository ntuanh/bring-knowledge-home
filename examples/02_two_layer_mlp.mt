# Two dense layers: each matmul absorbs its bias add and relu.
input  x  : [8, 16]
param  w1 : [16, 32]
param  b1 : [32]
param  w2 : [32, 4]
param  b2 : [4]
h1 = matmul(x, w1)
a1 = add(h1, b1)
z1 = relu(a1)
h2 = matmul(z1, w2)
y  = add(h2, b2)
output y
