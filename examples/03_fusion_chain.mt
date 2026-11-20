# Element-wise chain: y = relu((x - mean) * gain) * 0.5 + x in a single loop.
input  x    : [64, 32]
param  mean : [32]
param  gain : [32]
centered = sub(x, mean)
scaled   = mul(centered, gain)
act      = relu(scaled)
half     = scale(act, 0.5)
y        = add(half, x)
output y
