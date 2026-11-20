# Compiler Visualizer

Learn how compilers work by inspecting tokens, ASTs, symbol tables, IR transformations, and generated assembly.

## Layout

```text
compilers/
  minic/          C compiler: source, headers, examples, and tests
  expression/     Independent expression compiler for the adapter demo
visualizer/       HTTP server, compiler adapters, frontend, and API tests
scripts/          Port selection and browser checks
docs/             Usage and adapter protocol
```

AI compiler source is maintained separately on [`compiler/ai_compiler`](https://github.com/ntuanh/bring-knowledge-home/tree/compiler/ai_compiler). This branch builds and runs independently of that source. The repository's general learning goals are on [`main`](https://github.com/ntuanh/bring-knowledge-home).

## Run

Requires Python 3, GCC, Make, and GNU binutils on Linux x86-64.

```sh
make build
make run
make test
```

Open the URL printed by the server. The C Compiler view includes source-linked tokens, AST, scopes, IR, optimization logs, assembly, execution, and GCC/ELF inspection.

Compiler Lab provides a common adapter interface with Back/Next navigation and source highlighting. Built-in adapters cover Mini C and the standalone expression compiler. Lab navigation replays recorded results after compilation; it does not pause a running compiler process.

See the [usage guide](docs/compiler-visualizer.md) and [adapter protocol](docs/compiler-adapters.md) for details. Add another compiler through an external adapter rather than copying its implementation into this project.
