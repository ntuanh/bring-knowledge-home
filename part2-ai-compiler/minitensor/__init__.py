"""minitensor: a tiny AI/tensor compiler for learning.

Pipeline (one module per stage):

    .mt text --parser.py--> AST --graph.py--> graph IR with shapes
        --passes.py--> optimized graph + fusion groups
        --loopir.py--> loop IR (kernels) --codegen_c.py--> C source
        --driver.py--> gcc -O2, run, compare with interp.py (reference)
"""
