/*
 * codegen.c - stage 6: code generation.
 *
 * Role: translates the IR into x86-64 assembly text (GNU assembler, Intel
 * syntax) that follows the System V AMD64 calling convention.
 * Input: an IrProgram (optimized or not).
 * Output: assembly text in a StrBuf. This is the last thing minic produces:
 *         `as` turns the text into an object file and `gcc`/`ld` link it.
 * Called by: main.c (run_pipeline).
 *
 * Strategy (deliberately simple, similar to `gcc -O0`):
 *   - every variable and temporary lives in its own 4-byte stack slot,
 *     addressed as DWORD PTR [rbp-N];
 *   - each IR instruction loads its operands into eax, computes, and stores
 *     the result back, so no register holds a value between instructions;
 *   - before each group of machine instructions a `# <IR instruction>`
 *     comment shows which IR instruction it implements.
 *
 * Stack frame of a function:
 *
 *     [rbp+8]   return address (pushed by `call`)
 *     [rbp]     caller's rbp   (pushed by `push rbp`)
 *     [rbp-4]   slot 1
 *     [rbp-8]   slot 2 ...
 *
 * At function entry rsp is 8 mod 16 (the ABI requires 16-byte alignment
 * *before* the call pushed the return address). `push rbp` restores 16-byte
 * alignment and the frame size is rounded up to a multiple of 16, so rsp is
 * 16-byte aligned at every `call`.
 */
#include "minic.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static StrBuf *out;
static const char **slots; /* slot i holds the name stored at [rbp - 4*(i+1)] */
static int nslots, slots_cap;
static bool uses_print;

static const char *const arg_regs[MAX_PARAMS] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};

static int slot_offset(const char *name)
{
    for (int i = 0; i < nslots; i++)
        if (strcmp(slots[i], name) == 0)
            return 4 * (i + 1);
    if (nslots == slots_cap) {
        slots_cap = slots_cap ? slots_cap * 2 : 32;
        slots = xrealloc(slots, (size_t)slots_cap * sizeof(char *));
    }
    slots[nslots++] = name;
    return 4 * nslots;
}

/* Assembly text of an operand: an immediate or a stack slot. */
static const char *opd(Operand o)
{
    if (o.kind == OPD_CONST)
        return xasprintf("%d", o.value);
    return xasprintf("DWORD PTR [rbp-%d]", slot_offset(o.name));
}

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    /* All instruction lines are indented by four spaces. */
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_printf(out, "    %s\n", buf);
    free(buf);
}

static void gen_binary(const Instr *in)
{
    const char *setcc = NULL;
    line("mov eax, %s", opd(in->a));
    switch (in->binop) {
    case TK_PLUS:
        line("add eax, %s", opd(in->b));
        break;
    case TK_MINUS:
        line("sub eax, %s", opd(in->b));
        break;
    case TK_STAR:
        if (in->b.kind == OPD_CONST)
            line("imul eax, eax, %s", opd(in->b));
        else
            line("imul eax, %s", opd(in->b));
        break;
    case TK_SLASH:
    case TK_PERCENT:
        /* idiv divides edx:eax, so cdq first sign-extends eax into edx.
         * The quotient lands in eax and the remainder in edx. */
        line("cdq");
        if (in->b.kind == OPD_CONST) {
            line("mov ecx, %s", opd(in->b)); /* idiv has no immediate form */
            line("idiv ecx");
        } else {
            line("idiv %s", opd(in->b));
        }
        if (in->binop == TK_PERCENT)
            line("mov eax, edx");
        break;
    case TK_EQ: setcc = "sete"; break;
    case TK_NE: setcc = "setne"; break;
    case TK_LT: setcc = "setl"; break;
    case TK_LE: setcc = "setle"; break;
    case TK_GT: setcc = "setg"; break;
    case TK_GE: setcc = "setge"; break;
    default: break;
    }
    if (setcc) {
        /* A comparison produces 0 or 1: cmp sets the flags, setcc writes one byte. */
        line("cmp eax, %s", opd(in->b));
        line("%s al", setcc);
        line("movzx eax, al");
    }
    line("mov %s, eax", opd(in->dst));
}

static void gen_instr(const Instr *in)
{
    StrBuf text;
    sb_init(&text);
    instr_text(in, &text);
    line("# %s", text.data);

    switch (in->op) {
    case IR_COPY:
        if (in->a.kind == OPD_CONST) {
            line("mov %s, %s", opd(in->dst), opd(in->a));
        } else {
            line("mov eax, %s", opd(in->a)); /* x86 cannot move memory to memory directly */
            line("mov %s, eax", opd(in->dst));
        }
        break;
    case IR_BIN:
        gen_binary(in);
        break;
    case IR_UN:
        line("mov eax, %s", opd(in->a));
        if (in->binop == TK_MINUS) {
            line("neg eax");
        } else { /* logical not: 1 if the value is zero, else 0 */
            line("test eax, eax");
            line("sete al");
            line("movzx eax, al");
        }
        line("mov %s, eax", opd(in->dst));
        break;
    case IR_LABEL:
        sb_printf(out, ".L%d:\n", in->label);
        break;
    case IR_JUMP:
        line("jmp .L%d", in->label);
        break;
    case IR_BRZ:
        if (in->a.kind == OPD_CONST) {
            line("mov eax, %s", opd(in->a));
            line("test eax, eax");
        } else {
            line("cmp %s, 0", opd(in->a));
        }
        line("je .L%d", in->label);
        break;
    case IR_CALL:
        if (strcmp(in->fname, "print") == 0) {
            /* print(x) is compiled to printf("%d\n", x) from libc. printf is
             * variadic, and the ABI wants the number of vector registers used
             * in al, hence `mov eax, 0`. */
            uses_print = true;
            line("mov esi, %s", opd(in->args[0]));
            line("lea rdi, .LCprint[rip]");
            line("mov eax, 0");
            line("call printf@PLT");
            break;
        }
        for (int i = 0; i < in->nargs; i++)
            line("mov %s, %s", arg_regs[i], opd(in->args[i]));
        line("call %s", in->fname);
        if (in->dst.kind != OPD_NONE)
            line("mov %s, eax", opd(in->dst));
        break;
    case IR_RET:
        if (in->a.kind != OPD_NONE)
            line("mov eax, %s", opd(in->a));
        line("leave"); /* mov rsp, rbp ; pop rbp */
        line("ret");
        break;
    case IR_NOP:
        break;
    }
}

static void gen_function(const IrFunc *fn)
{
    nslots = 0;
    /* The body is generated into a separate buffer first, because the frame
     * size is only known once every slot has been assigned. */
    StrBuf *saved = out;
    StrBuf body;
    sb_init(&body);
    out = &body;
    for (int i = 0; i < fn->nparams; i++)
        line("mov %s, %s", opd((Operand){OPD_VAR, 0, fn->params[i]}), arg_regs[i]);
    for (int i = 0; i < fn->count; i++)
        gen_instr(&fn->ins[i]);
    out = saved;

    int frame = (4 * nslots + 15) / 16 * 16;
    sb_printf(out, "\n    .globl %s\n    .type %s, @function\n%s:\n", fn->name, fn->name, fn->name);
    line("push rbp");
    line("mov rbp, rsp");
    if (frame)
        line("sub rsp, %d", frame);
    if (fn->nparams)
        line("# store the %d register argument%s in stack slots", fn->nparams, fn->nparams == 1 ? "" : "s");
    sb_puts(out, body.data);
    sb_printf(out, "    .size %s, .-%s\n", fn->name, fn->name);
}

void codegen(const IrProgram *prog, StrBuf *output)
{
    out = output;
    uses_print = false;
    sb_puts(out, "    .intel_syntax noprefix\n    .text\n");
    for (int i = 0; i < prog->count; i++)
        gen_function(prog->funcs[i]);
    if (uses_print)
        sb_puts(out, "\n    .section .rodata\n.LCprint:\n    .string \"%d\\n\"\n");
    /* Tells the linker this object does not need an executable stack. */
    sb_puts(out, "\n    .section .note.GNU-stack,\"\",@progbits\n");
}
