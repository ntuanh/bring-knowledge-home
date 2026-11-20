/*
 * ir.c - stage 4: intermediate representation (three-address code).
 *
 * Role: flattens the tree-shaped AST into a linear list of simple
 * instructions, each with at most one operator, plus labels and jumps for
 * control flow. This form is easy to optimize and easy to turn into assembly.
 * Input: the checked and annotated AST from sema.c.
 * Output: an IrProgram (one IrFunc per function).
 * Called by: main.c (run_pipeline). The IR is consumed by opt.c and codegen.c.
 *
 * Example:  return a + b * 2;   becomes   t1 = b * 2
 *                                         t2 = a + t1
 *                                         return t2
 */
#include "minic.h"

#include <string.h>

static IrFunc *fn;        /* function being generated */
static int label_counter; /* labels are unique in the whole program */

static Instr *emit(IrOp op, int line)
{
    if (fn->count == fn->cap) {
        fn->cap = fn->cap ? fn->cap * 2 : 32;
        fn->ins = xrealloc(fn->ins, (size_t)fn->cap * sizeof(Instr));
    }
    Instr *in = &fn->ins[fn->count++];
    memset(in, 0, sizeof *in);
    in->op = op;
    in->line = line;
    return in;
}

static Operand constant(int32_t value) { return (Operand){OPD_CONST, value, NULL}; }
static Operand variable(const char *name) { return (Operand){OPD_VAR, 0, name}; }
static Operand new_temp(void) { return (Operand){OPD_TEMP, 0, xasprintf("t%d", ++fn->ntemps)}; }
static int new_label(void) { return ++label_counter; }

static void emit_label(int label, int line) { emit(IR_LABEL, line)->label = label; }
static void emit_jump(int label, int line) { emit(IR_JUMP, line)->label = label; }

/*
 * Generates code for an expression and returns the operand holding its value.
 * `want`, if not NULL, is where the caller would like the result (the target
 * of an assignment). It is only a hint: when the expression is a plain
 * constant or variable, that operand is returned and the caller copies it.
 */
static Operand gen_expr(const Node *n, const Operand *want)
{
    switch (n->kind) {
    case ND_NUMBER:
        return constant(n->value);
    case ND_VAR:
        return variable(n->ir_name);
    case ND_UNARY: {
        if (n->op == TK_PLUS)
            return gen_expr(n->kids[0], want); /* unary plus does nothing */
        Operand a = gen_expr(n->kids[0], NULL);
        Operand dst = want ? *want : new_temp();
        Instr *in = emit(IR_UN, n->line);
        in->binop = n->op;
        in->dst = dst;
        in->a = a;
        return dst;
    }
    case ND_BINARY: {
        Operand a = gen_expr(n->kids[0], NULL);
        Operand b = gen_expr(n->kids[1], NULL);
        Operand dst = want ? *want : new_temp();
        Instr *in = emit(IR_BIN, n->line);
        in->binop = n->op;
        in->dst = dst;
        in->a = a;
        in->b = b;
        return dst;
    }
    case ND_ASSIGN: {
        Operand target = variable(n->kids[0]->ir_name);
        Operand value = gen_expr(n->kids[1], &target);
        if (!operand_eq(value, target)) {
            Instr *in = emit(IR_COPY, n->line);
            in->dst = target;
            in->a = value;
        }
        return target; /* the value of `a = b` is the new value of a */
    }
    case ND_CALL: {
        Operand args[MAX_PARAMS];
        for (int i = 0; i < n->nkids; i++)
            args[i] = gen_expr(n->kids[i], NULL);
        Operand dst = {OPD_NONE, 0, NULL};
        if (n->type != TY_VOID)
            dst = want ? *want : new_temp();
        Instr *in = emit(IR_CALL, n->line);
        in->fname = n->name;
        in->nargs = n->nkids;
        memcpy(in->args, args, sizeof(Operand) * (size_t)n->nkids);
        in->dst = dst;
        return dst;
    }
    default:
        return constant(0);
    }
}

static void gen_stmt(const Node *n)
{
    switch (n->kind) {
    case ND_VAR_DECL:
        if (n->nkids) {
            Operand target = variable(n->ir_name);
            Operand value = gen_expr(n->kids[0], &target);
            if (!operand_eq(value, target)) {
                Instr *in = emit(IR_COPY, n->line);
                in->dst = target;
                in->a = value;
            }
        }
        break;
    case ND_RETURN: {
        Operand value = {OPD_NONE, 0, NULL};
        if (n->nkids)
            value = gen_expr(n->kids[0], NULL);
        emit(IR_RET, n->line)->a = value;
        break;
    }
    case ND_IF: {
        /*     branch_if_zero cond, Lelse
         *     <then>
         *     jump Lend
         *   Lelse:
         *     <else>
         *   Lend:                                  */
        bool has_else = n->nkids > 2;
        int l_else = new_label();
        int l_end = has_else ? new_label() : l_else;
        Operand cond = gen_expr(n->kids[0], NULL);
        Instr *br = emit(IR_BRZ, n->line);
        br->a = cond;
        br->label = l_else;
        gen_stmt(n->kids[1]);
        if (has_else) {
            emit_jump(l_end, n->line);
            emit_label(l_else, n->kids[2]->line);
            gen_stmt(n->kids[2]);
        }
        emit_label(l_end, n->end_line);
        break;
    }
    case ND_WHILE: {
        /*   Lstart:
         *     branch_if_zero cond, Lend
         *     <body>
         *     jump Lstart
         *   Lend:                                  */
        int l_start = new_label();
        int l_end = new_label();
        emit_label(l_start, n->line);
        Operand cond = gen_expr(n->kids[0], NULL);
        Instr *br = emit(IR_BRZ, n->line);
        br->a = cond;
        br->label = l_end;
        gen_stmt(n->kids[1]);
        emit_jump(l_start, n->line);
        emit_label(l_end, n->end_line);
        break;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->nkids; i++)
            gen_stmt(n->kids[i]);
        break;
    case ND_EXPR_STMT:
        gen_expr(n->kids[0], NULL);
        break;
    default:
        break;
    }
}

static IrFunc *gen_function(const Node *node)
{
    fn = xcalloc(1, sizeof(IrFunc));
    fn->name = node->name;
    fn->ret_type = node->ret_type;
    fn->nparams = node->nkids - 1;
    for (int i = 0; i < fn->nparams; i++)
        fn->params[i] = node->kids[i]->ir_name;
    const Node *body = node->kids[node->nkids - 1];
    gen_stmt(body);
    /* Control may reach the end of the body: add an implicit return
     * (`return 0` for int functions, as C does for main). */
    if (fn->count == 0 || fn->ins[fn->count - 1].op != IR_RET) {
        Instr *ret = emit(IR_RET, body->end_line);
        if (fn->ret_type == TY_INT)
            ret->a = constant(0);
    }
    return fn;
}

IrProgram *ir_generate(const Node *program)
{
    IrProgram *prog = xcalloc(1, sizeof(IrProgram));
    prog->funcs = xcalloc((size_t)program->nkids, sizeof(IrFunc *));
    label_counter = 0;
    for (int i = 0; i < program->nkids; i++)
        prog->funcs[prog->count++] = gen_function(program->kids[i]);
    return prog;
}

IrProgram *ir_clone(const IrProgram *prog)
{
    IrProgram *copy = xcalloc(1, sizeof(IrProgram));
    copy->count = prog->count;
    copy->funcs = xcalloc((size_t)prog->count, sizeof(IrFunc *));
    for (int i = 0; i < prog->count; i++) {
        IrFunc *f = xmalloc(sizeof(IrFunc));
        *f = *prog->funcs[i];
        f->cap = f->count;
        f->ins = xmalloc((size_t)f->count * sizeof(Instr));
        memcpy(f->ins, prog->funcs[i]->ins, (size_t)f->count * sizeof(Instr));
        copy->funcs[i] = f;
    }
    return copy;
}

bool operand_eq(Operand x, Operand y)
{
    if (x.kind != y.kind)
        return false;
    if (x.kind == OPD_CONST)
        return x.value == y.value;
    if (x.kind == OPD_NONE)
        return true;
    return strcmp(x.name, y.name) == 0;
}

void operand_text(Operand o, StrBuf *sb)
{
    if (o.kind == OPD_CONST)
        sb_printf(sb, "%d", o.value);
    else if (o.kind != OPD_NONE)
        sb_puts(sb, o.name);
}

void instr_text(const Instr *in, StrBuf *sb)
{
    switch (in->op) {
    case IR_NOP:
        sb_puts(sb, "nop");
        break;
    case IR_COPY:
        operand_text(in->dst, sb);
        sb_puts(sb, " = ");
        operand_text(in->a, sb);
        break;
    case IR_BIN:
        operand_text(in->dst, sb);
        sb_puts(sb, " = ");
        operand_text(in->a, sb);
        sb_printf(sb, " %s ", token_kind_text(in->binop));
        operand_text(in->b, sb);
        break;
    case IR_UN:
        operand_text(in->dst, sb);
        sb_printf(sb, " = %s", token_kind_text(in->binop));
        operand_text(in->a, sb);
        break;
    case IR_LABEL:
        sb_printf(sb, "L%d:", in->label);
        break;
    case IR_JUMP:
        sb_printf(sb, "jump L%d", in->label);
        break;
    case IR_BRZ:
        sb_puts(sb, "branch_if_zero ");
        operand_text(in->a, sb);
        sb_printf(sb, ", L%d", in->label);
        break;
    case IR_CALL:
        if (in->dst.kind != OPD_NONE) {
            operand_text(in->dst, sb);
            sb_puts(sb, " = ");
        }
        sb_printf(sb, "call %s(", in->fname);
        for (int i = 0; i < in->nargs; i++) {
            if (i)
                sb_puts(sb, ", ");
            operand_text(in->args[i], sb);
        }
        sb_putc(sb, ')');
        break;
    case IR_RET:
        sb_puts(sb, "return");
        if (in->a.kind != OPD_NONE) {
            sb_putc(sb, ' ');
            operand_text(in->a, sb);
        }
        break;
    }
}

void ir_print(const IrProgram *prog, StrBuf *sb)
{
    for (int f = 0; f < prog->count; f++) {
        const IrFunc *func = prog->funcs[f];
        sb_printf(sb, "%sfunction %s(", f ? "\n" : "", func->name);
        for (int i = 0; i < func->nparams; i++)
            sb_printf(sb, "%s%s", i ? ", " : "", func->params[i]);
        sb_printf(sb, ") -> %s:\n", func->ret_type == TY_VOID ? "void" : "int");
        for (int i = 0; i < func->count; i++) {
            sb_puts(sb, func->ins[i].op == IR_LABEL ? "  " : "    ");
            instr_text(&func->ins[i], sb);
            sb_putc(sb, '\n');
        }
    }
}
