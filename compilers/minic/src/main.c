/*
 * main.c - the minic driver.
 *
 * Role: parses the command line, reads the source file and runs the stages
 * in order, stopping at the first stage that reports errors.
 * Input: a C-subset source file.
 * Output: one of the intermediate forms (--emit=...), every stage as JSON
 *         (--json), or by default x86-64 assembly text.
 * Called by: the user, the test script, and visualizer/pipelines.py.
 *
 * Usage: minic [-O0|-O1] [--emit=tokens|ast|ir|ir-opt|asm] [--json] [-o out.s] file.c
 *
 * minic never produces an object file or an executable. To get a program:
 *     minic prog.c -o prog.s && as prog.s -o prog.o && gcc prog.o -o prog
 */
#include "minic.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SOURCE_BYTES (1024 * 1024)

static void usage(void)
{
    fprintf(stderr, "usage: minic [-O0|-O1] [--emit=tokens|ast|ir|ir-opt|asm] [--json] [-o out.s] file.c\n");
    exit(2);
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "minic: cannot open '%s'\n", path);
        exit(2);
    }
    char *buf = xmalloc(MAX_SOURCE_BYTES + 2);
    size_t n = fread(buf, 1, MAX_SOURCE_BYTES + 1, fp);
    fclose(fp);
    if (n > MAX_SOURCE_BYTES) {
        fprintf(stderr, "minic: '%s' is larger than %d bytes\n", path, MAX_SOURCE_BYTES);
        exit(2);
    }
    if (memchr(buf, '\0', n)) {
        fprintf(stderr, "minic: '%s' contains a NUL byte\n", path);
        exit(2);
    }
    buf[n] = '\0';
    return buf;
}

/* Runs the stages in order and records the first one that fails. */
static void run_pipeline(Compilation *c, const char *source, bool optimize_ir)
{
    /* Stage 1: characters -> tokens */
    bool ok = lex(source, &c->tokens, &c->diags);
    c->have_tokens = true; /* tokens before the error are still worth showing */
    if (!ok) {
        c->failed_stage = "lexer";
        return;
    }
    /* Stage 2: tokens -> AST */
    c->ast = parse(&c->tokens, &c->diags);
    if (!c->ast) {
        c->failed_stage = "parser";
        return;
    }
    /* Stage 3: names, scopes and types */
    ok = analyze(c->ast, &c->symbols, &c->diags);
    c->have_symbols = true;
    if (!ok) {
        c->failed_stage = "sema";
        return;
    }
    /* Stage 4: AST -> three-address IR */
    c->ir = ir_generate(c->ast);
    /* Stage 5: optimize a copy, so both versions can be shown */
    if (optimize_ir) {
        c->ir_opt = ir_clone(c->ir);
        optimize(c->ir_opt, &c->opt_log);
    }
    /* Stage 6: IR -> assembly text */
    sb_init(&c->asm_text);
    codegen(c->opt_level >= 1 ? c->ir_opt : c->ir, &c->asm_text);
    c->have_asm = true;
}

int main(int argc, char **argv)
{
    Compilation c = {0};
    const char *emit = "asm";
    const char *out_path = NULL;
    bool json = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-O0") == 0)
            c.opt_level = 0;
        else if (strcmp(arg, "-O1") == 0)
            c.opt_level = 1;
        else if (strncmp(arg, "--emit=", 7) == 0)
            emit = arg + 7;
        else if (strcmp(arg, "--json") == 0)
            json = true;
        else if (strcmp(arg, "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (arg[0] == '-')
            usage();
        else if (!c.filename)
            c.filename = arg;
        else
            usage();
    }
    if (!c.filename)
        usage();
    bool emit_ok = false;
    const char *const emits[] = {"tokens", "ast", "ir", "ir-opt", "asm"};
    for (size_t i = 0; i < sizeof emits / sizeof emits[0]; i++)
        emit_ok |= strcmp(emit, emits[i]) == 0;
    if (!emit_ok)
        usage();

    const char *source = read_file(c.filename);
    /* --emit=ir-opt always needs the optimizer, whatever the -O level. */
    run_pipeline(&c, source, c.opt_level >= 1 || strcmp(emit, "ir-opt") == 0);

    if (json) {
        json_emit(stdout, &c);
        return c.failed_stage ? 1 : 0;
    }

    for (int i = 0; i < c.diags.count; i++) {
        const Diag *d = &c.diags.items[i];
        fprintf(stderr, "%s:%d:%d: error: %s\n", c.filename, d->line, d->col, d->message);
    }

    StrBuf text;
    sb_init(&text);
    if (strcmp(emit, "tokens") == 0) {
        /* Tokens are printed even after a lexer error, up to the error. */
        for (int i = 0; i < c.tokens.count; i++) {
            const Token *t = &c.tokens.items[i];
            sb_printf(&text, "%d:%d\t", t->line, t->col);
            token_display(t, &text);
            sb_putc(&text, '\n');
        }
    } else if (c.failed_stage) {
        return 1;
    } else if (strcmp(emit, "ast") == 0) {
        ast_print(c.ast, 0, &text);
    } else if (strcmp(emit, "ir") == 0) {
        ir_print(c.ir, &text);
    } else if (strcmp(emit, "ir-opt") == 0) {
        ir_print(c.ir_opt, &text);
    } else {
        sb_puts(&text, c.asm_text.data);
    }

    FILE *fp = stdout;
    if (out_path) {
        fp = fopen(out_path, "w");
        if (!fp) {
            fprintf(stderr, "minic: cannot write '%s'\n", out_path);
            return 2;
        }
    }
    fputs(text.data, fp);
    if (out_path)
        fclose(fp);
    return c.failed_stage ? 1 : 0;
}
