# AI Compiler

Learning notes and reference implementations for understanding, rebuilding, and optimizing AI compilers.

## What to learn

Study how tensor programs become executable kernels:

```text
Model / tensor DSL → graph IR → optimization → loop IR → code generation → runtime
```

Focus on shape inference, broadcasting, constant folding, dead code elimination, operator fusion, memory layout, and loop tiling. Record IR before and after each pass, verify numerical correctness, and measure performance against a baseline.

## Example compilers and references

| Project | What to study | Documentation | Source |
| --- | --- | --- | --- |
| MLIR Toy | A teaching compiler: AST, custom dialects, shape inference, and lowering to LLVM. | [Tutorial](https://mlir.llvm.org/docs/Tutorials/Toy/) | [Examples](https://github.com/llvm/llvm-project/tree/main/mlir/examples/toy) |
| Apache TVM | Model graphs, tensor IR, fusion, scheduling, and code generation. | [Docs](https://tvm.apache.org/docs/) | [Repository](https://github.com/apache/tvm) |
| OpenXLA / XLA | Model compilation, HLO optimizations, and hardware backends. | [Docs](https://openxla.org/xla) | [Repository](https://github.com/openxla/xla) |
| Triton | GPU kernel compilation; vector addition, fused softmax, and tiled matrix multiplication. | [Tutorials](https://triton-lang.org/main/getting-started/tutorials/index.html) | [Repository](https://github.com/triton-lang/triton) |
| tinygrad | A tensor framework with a compiler: lazy computation and kernel generation. | [Docs](https://docs.tinygrad.org/) | [Repository](https://github.com/tinygrad/tinygrad) |

Suggested reading order: **MLIR Toy → tinygrad → TVM → XLA**, with **Triton** for GPU kernel experiments.

## Build a small compiler

1. Support `matmul`, `add`, and `relu` with shape checking and a reference interpreter.
2. Lower tensor operations to loops and generate C; compare outputs with the interpreter.
3. Add constant folding and fusion, then measure kernel count, memory use, and execution time before and after optimization.

An existing learning prototype, [MiniTensor](https://github.com/ntuanh/bring-knowledge-home/tree/compiler/compiler_visualizer/part2-ai-compiler), lives on `compiler/compiler_visualizer`. Use its parser, graph passes, loop IR, and C code generator as a starting point.
