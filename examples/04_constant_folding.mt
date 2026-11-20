# Constant folding and dead node elimination happen before any loop exists.
input  x : [4, 3]
const  weights : [3] = [1.0, 2.0, 3.0]
const  offset  : [3] = [0.5, 0.5, 0.5]
k      = add(weights, offset)      # only consts: folded to [1.5, 2.5, 3.5]
k2     = scale(k, 2.0)             # folded again: [3, 5, 7]
unused = mul(x, x)                 # no output depends on it: removed
y      = mul(x, k2)
output y
