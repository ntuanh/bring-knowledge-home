# Compiler Visualizer guide

Build and start the visualizer from this branch's root:

```sh
make build
make run
```

The server selects a free port in 8100–8999 unless configured otherwise. Requirements: Python 3, GCC, Make, and GNU binutils on Linux x86-64.

## Explore a compilation

- **C Compiler:** edit a sample, then inspect tokens, AST, scopes, IR, optimization, assembly, execution, or GCC/ELF output.
- **Compiler Lab:** choose Mini C, enable O1, and inspect optimizer before/after events. Choose the expression compiler and enter `2 + 3 * (4 - 1)` to trace stack bytecode and constant folding to `PUSH 11`.

Back/Next navigation replays compilation results. Mini C AST/IR events traverse result snapshots; optimization events use actual transformation logs. Live process breakpoints are not implemented.

## Source locations

- `compilers/minic/`: C implementation, examples, and compiler tests.
- `compilers/expression/compiler.py`: standalone adapter demonstration.
- `visualizer/`: server, adapters, frontend, and API tests.
- [`compiler/ai_compiler`](https://github.com/ntuanh/bring-knowledge-home/tree/compiler/ai_compiler): MiniTensor source, examples, tests, and VLM roadmap, maintained separately.

## Extend and verify

Follow the [adapter protocol](compiler-adapters.md) to connect an external compiler. It must export the stage outputs or events you want to inspect.

```sh
make test
python3 scripts/ui_check.py http://127.0.0.1:8100 /tmp/compiler-visualizer-screenshots
```

The browser checks require Firefox. Change the URL to the port printed by your running server.
