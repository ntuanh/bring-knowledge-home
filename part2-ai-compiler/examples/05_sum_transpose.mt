# Gram matrix and row sums: matmul, transpose and sum (a reduction) together.
input  x : [6, 4]
xt   = transpose(x)                # [4, 6]
gram = matmul(xt, x)               # [4, 4]
half = scale(gram, 0.5)
rows = sum(half, 1)                # [4]: sum over each row
output half
output rows
