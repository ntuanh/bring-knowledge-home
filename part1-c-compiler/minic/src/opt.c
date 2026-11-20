/*
 * opt.c - stage 5: the optimizer (runs only at -O1).
 *
 * Role: rewrites the IR into an equivalent but cheaper IR.
 * Input: an IrProgram from ir.c (main.c passes a clone, so the unoptimized
 *        IR stays available for display).
 * Output: the same IrProgram modified in place, plus an OptLog with one
 *         "before -> after" entry for every change.
 * Called by: main.c (run_pipeline). The result is consumed by codegen.c.
 *
 * The passes run one after another, and the whole sequence is repeated until
 * no pass changes anything (a "fixed point"), because one pass often creates
 * work for another: propagation exposes constants to fold, folding exposes
 * constant branches, removing branches exposes unreachable code, and so on.
 *
 *   pass                          scope            function
 *   constant / copy propagation   basic block      pass_propagate()
 *   constant folding              instruction      pass_fold()
 *   algebraic simplification      instruction      pass_algebraic()
 *   common subexpression elim.    basic block      pass_cse()
 *   branch folding                instruction      pass_branch_fold()
 *   unreachable code elimination  function         pass_unreachable()
 *   unused label removal          function         pass_unused_labels()
 *   jump-to-next-label removal    function         pass_jump_to_next()
 *   dead store elimination        function         pass_dead_stores()
 *
 * A "basic block" is a straight run of instructions with no label in the
 * middle; facts learned inside it cannot be invalidated by a jump from
 * elsewhere, which is why the block-local passes forget everything at labels.
 */
#include "minic.h"

#include <stdlib.h>
#include <string.h>

static OptLog *oplog;
static IrFunc *fn;
static bool changed;

/* ------------------------------------------------------------- utilities */

static char *text_of(const Instr *in)
{
    StrBuf sb;
    sb_init(&sb);
    instr_text(in, &sb);
    return sb.data;
}

/* Records a change. `after` == NULL means the instruction was removed. */
static void record(const char *pass, const Instr *before, const Instr *after)
{
    if (oplog->count == oplog->cap) {
        oplog->cap = oplog->cap ? oplog->cap * 2 : 32;
        oplog->items = xrealloc(oplog->items, (size_t)oplog->cap * sizeof(OptEntry));
    }
    oplog->items[oplog->count++] =
        (OptEntry){pass, fn->name, text_of(before), after ? text_of(after) : xstrdup("(removed)")};
    changed = true;
}

static void remove_instr(const char *pass, Instr *in)
{
    record(pass, in, NULL);
    in->op = IR_NOP;
}

/* Deletes IR_NOP instructions. */
static void compact(void)
{
    int n = 0;
    for (int i = 0; i < fn->count; i++)
        if (fn->ins[i].op != IR_NOP)
            fn->ins[n++] = fn->ins[i];
    fn->count = n;
}

static bool is_name(Operand o) { return o.kind == OPD_VAR || o.kind == OPD_TEMP; }
static bool is_const(Operand o) { return o.kind == OPD_CONST; }
static Operand constant(int32_t v) { return (Operand){OPD_CONST, v, NULL}; }

/* Does this instruction write a variable or temporary? */
static bool has_dst(const Instr *in)
{
    return in->op == IR_COPY || in->op == IR_BIN || in->op == IR_UN || (in->op == IR_CALL && in->dst.kind != OPD_NONE);
}

/* Collects pointers to every operand the instruction reads. */
static int used_operands(Instr *in, Operand **out)
{
    int n = 0;
    switch (in->op) {
    case IR_COPY:
    case IR_UN:
    case IR_BRZ:
        out[n++] = &in->a;
        break;
    case IR_BIN:
        out[n++] = &in->a;
        out[n++] = &in->b;
        break;
    case IR_CALL:
        for (int i = 0; i < in->nargs; i++)
            out[n++] = &in->args[i];
        break;
    case IR_RET:
        if (in->a.kind != OPD_NONE)
            out[n++] = &in->a;
        break;
    default:
        break;
    }
    return n;
}

/* Turns an instruction into `dst = value`. */
static void make_copy(Instr *in, Operand value)
{
    in->op = IR_COPY;
    in->a = value;
    in->b = (Operand){OPD_NONE, 0, NULL};
}

/*
 * Evaluates `a op b` at compile time with the same 32-bit wrap-around the
 * generated machine code has. Returns false when folding is not allowed:
 * division or modulo by zero, and INT_MIN / -1, trap at run time and must
 * keep doing so.
 */
static bool fold_binary(TokenKind op, int32_t a, int32_t b, int32_t *out)
{
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (op) {
    case TK_PLUS:  *out = (int32_t)(ua + ub); return true;
    case TK_MINUS: *out = (int32_t)(ua - ub); return true;
    case TK_STAR:  *out = (int32_t)(ua * ub); return true;
    case TK_SLASH:
    case TK_PERCENT:
        if (b == 0 || (a == INT32_MIN && b == -1))
            return false;
        *out = (op == TK_SLASH) ? a / b : a % b;
        return true;
    case TK_EQ: *out = a == b; return true;
    case TK_NE: *out = a != b; return true;
    case TK_LT: *out = a < b;  return true;
    case TK_LE: *out = a <= b; return true;
    case TK_GT: *out = a > b;  return true;
    case TK_GE: *out = a >= b; return true;
    default:    return false;
    }
}

/* ------------------------------------- constant and copy propagation */

/*
 * Walks each basic block remembering facts of the form "x currently equals
 * v" (v a constant or another name), learned from `x = v`. Later reads of x
 * in the same block are replaced by v. A fact dies when x or v is assigned.
 */
static void pass_propagate(void)
{
    struct { Operand key, val; } *facts = xmalloc((size_t)(fn->count + 1) * sizeof *facts);
    int nfacts = 0;

    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op == IR_LABEL) {
            nfacts = 0; /* another path may jump here: forget everything */
            continue;
        }
        Operand *use[MAX_PARAMS + 2];
        int nuse = used_operands(in, use);
        for (int u = 0; u < nuse; u++) {
            if (!is_name(*use[u]))
                continue;
            for (int k = 0; k < nfacts; k++) {
                if (operand_eq(facts[k].key, *use[u])) {
                    Instr before = *in;
                    *use[u] = facts[k].val;
                    record(is_const(facts[k].val) ? "constant propagation" : "copy propagation", &before, in);
                    break;
                }
            }
        }
        if (has_dst(in)) {
            int n = 0;
            for (int k = 0; k < nfacts; k++)
                if (!operand_eq(facts[k].key, in->dst) && !operand_eq(facts[k].val, in->dst))
                    facts[n++] = facts[k];
            nfacts = n;
            if (in->op == IR_COPY && !operand_eq(in->a, in->dst)) {
                facts[nfacts].key = in->dst;
                facts[nfacts].val = in->a;
                nfacts++;
            }
        }
    }
    free(facts);
}

/* ----------------------------------------------------- constant folding */

static void pass_fold(void)
{
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        int32_t value;
        if (in->op == IR_BIN && is_const(in->a) && is_const(in->b)) {
            if (!fold_binary(in->binop, in->a.value, in->b.value, &value))
                continue;
        } else if (in->op == IR_UN && is_const(in->a)) {
            value = (in->binop == TK_BANG) ? (in->a.value == 0) : (int32_t)(0u - (uint32_t)in->a.value);
        } else {
            continue;
        }
        Instr before = *in;
        make_copy(in, constant(value));
        record("constant folding", &before, in);
    }
}

/* --------------------------------------------- algebraic simplification */

static bool is_value(Operand o, int32_t v) { return is_const(o) && o.value == v; }

static void pass_algebraic(void)
{
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op == IR_COPY && operand_eq(in->a, in->dst)) {
            remove_instr("algebraic simplification", in); /* x = x */
            continue;
        }
        if (in->op != IR_BIN)
            continue;
        Operand a = in->a, b = in->b, result = {OPD_NONE, 0, NULL};
        switch (in->binop) {
        case TK_PLUS:
            if (is_value(b, 0)) result = a;         /* x + 0 = x */
            else if (is_value(a, 0)) result = b;    /* 0 + x = x */
            break;
        case TK_MINUS:
            if (is_value(b, 0)) result = a;         /* x - 0 = x */
            else if (is_name(a) && operand_eq(a, b)) result = constant(0); /* x - x = 0 */
            break;
        case TK_STAR:
            if (is_value(b, 1)) result = a;         /* x * 1 = x */
            else if (is_value(a, 1)) result = b;    /* 1 * x = x */
            else if (is_value(a, 0) || is_value(b, 0)) result = constant(0); /* x * 0 = 0 */
            break;
        case TK_SLASH:
            if (is_value(b, 1)) result = a;         /* x / 1 = x */
            break;
        default:
            break;
        }
        if (result.kind == OPD_NONE)
            continue;
        Instr before = *in;
        make_copy(in, result);
        record("algebraic simplification", &before, in);
    }
    compact();
}

/* ---------------------------------- local common subexpression elimination */

static bool commutative(TokenKind op) { return op == TK_PLUS || op == TK_STAR || op == TK_EQ || op == TK_NE; }

/*
 * Inside a basic block, if `a op b` was already computed into some name and
 * neither a, b nor that name changed since, reuse the earlier result.
 */
static void pass_cse(void)
{
    Instr *avail = xmalloc((size_t)(fn->count + 1) * sizeof(Instr));
    int navail = 0;

    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op == IR_LABEL) {
            navail = 0;
            continue;
        }
        if (in->op == IR_BIN) {
            for (int k = 0; k < navail; k++) {
                const Instr *e = &avail[k];
                bool same = e->binop == in->binop &&
                            ((operand_eq(e->a, in->a) && operand_eq(e->b, in->b)) ||
                             (commutative(in->binop) && operand_eq(e->a, in->b) && operand_eq(e->b, in->a)));
                if (same) {
                    Instr before = *in;
                    make_copy(in, e->dst);
                    record("common subexpression elimination", &before, in);
                    break;
                }
            }
        }
        if (has_dst(in)) {
            int n = 0;
            for (int k = 0; k < navail; k++)
                if (!operand_eq(avail[k].dst, in->dst) && !operand_eq(avail[k].a, in->dst) &&
                    !operand_eq(avail[k].b, in->dst))
                    avail[n++] = avail[k];
            navail = n;
            if (in->op == IR_BIN && !operand_eq(in->a, in->dst) && !operand_eq(in->b, in->dst))
                avail[navail++] = *in;
        }
    }
    free(avail);
}

/* ------------------------------------------------------- branch folding */

static void pass_branch_fold(void)
{
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op != IR_BRZ || !is_const(in->a))
            continue;
        if (in->a.value == 0) {
            Instr before = *in; /* the condition is always zero: always jump */
            in->op = IR_JUMP;
            in->a = (Operand){OPD_NONE, 0, NULL};
            record("branch folding", &before, in);
        } else {
            remove_instr("branch folding", in); /* never zero: never jump */
        }
    }
    compact();
}

/* ----------------------------------------- unreachable code elimination */

/* Code after an unconditional `jump` or `return` can only be reached through a label. */
static void pass_unreachable(void)
{
    bool reachable = true;
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op == IR_LABEL)
            reachable = true;
        else if (!reachable)
            remove_instr("unreachable code elimination", in);
        if (in->op == IR_JUMP || in->op == IR_RET)
            reachable = false;
    }
    compact();
}

/* A label that no jump refers to is removed; this merges two basic blocks. */
static void pass_unused_labels(void)
{
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op != IR_LABEL)
            continue;
        bool used = false;
        for (int j = 0; j < fn->count && !used; j++)
            used = (fn->ins[j].op == IR_JUMP || fn->ins[j].op == IR_BRZ) && fn->ins[j].label == in->label;
        if (!used)
            remove_instr("unused label removal", in);
    }
    compact();
}

/* `jump L` directly followed by `L:` does nothing. */
static void pass_jump_to_next(void)
{
    for (int i = 0; i < fn->count; i++) {
        Instr *in = &fn->ins[i];
        if (in->op != IR_JUMP)
            continue;
        for (int j = i + 1; j < fn->count && fn->ins[j].op == IR_LABEL; j++) {
            if (fn->ins[j].label == in->label) {
                remove_instr("jump to next label removal", in);
                break;
            }
        }
    }
    compact();
}

/* ------------------------------------------------ dead store elimination */

static int name_index(const char **names, int *count, const char *name)
{
    for (int i = 0; i < *count; i++)
        if (strcmp(names[i], name) == 0)
            return i;
    names[*count] = name;
    return (*count)++;
}

/*
 * Liveness analysis: a name is "live" after an instruction if some later
 * instruction may still read its current value. It is computed backwards
 * over the whole function (loops included) until the sets stop changing:
 *
 *     live_out[i] = union of live_in[s] for every successor s of i
 *     live_in[i]  = (live_out[i] - written by i) + read by i
 *
 * An assignment whose target is not live afterwards is a dead store.
 */
static void pass_dead_stores(void)
{
    int n = fn->count;
    if (n == 0)
        return;
    const char **names = xmalloc((size_t)(n * (MAX_PARAMS + 3) + 1) * sizeof(char *));
    int nnames = 0;
    for (int i = 0; i < n; i++) {
        Instr *in = &fn->ins[i];
        Operand *use[MAX_PARAMS + 2];
        int nuse = used_operands(in, use);
        for (int u = 0; u < nuse; u++)
            if (is_name(*use[u]))
                name_index(names, &nnames, use[u]->name);
        if (has_dst(in))
            name_index(names, &nnames, in->dst.name);
    }
    /* The analysis needs n * nnames flags; skip it for absurdly large functions. */
    if (nnames == 0 || (size_t)n * (size_t)nnames > 64u * 1024 * 1024) {
        free(names);
        return;
    }

    /* target[i] = index of the instruction a jump/branch at i goes to */
    int *target = xmalloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) {
        target[i] = -1;
        if (fn->ins[i].op == IR_JUMP || fn->ins[i].op == IR_BRZ)
            for (int j = 0; j < n; j++)
                if (fn->ins[j].op == IR_LABEL && fn->ins[j].label == fn->ins[i].label)
                    target[i] = j;
    }

    bool *live_in = xcalloc((size_t)n * (size_t)nnames, sizeof(bool));
    bool *live_out = xcalloc((size_t)n * (size_t)nnames, sizeof(bool));
    bool *row = xmalloc((size_t)nnames * sizeof(bool));
    bool again = true;
    while (again) {
        again = false;
        for (int i = n - 1; i >= 0; i--) {
            Instr *in = &fn->ins[i];
            bool *out = &live_out[(size_t)i * (size_t)nnames];
            memset(out, 0, (size_t)nnames * sizeof(bool));
            if (in->op != IR_RET) {
                if (in->op != IR_JUMP && i + 1 < n)
                    for (int v = 0; v < nnames; v++)
                        out[v] |= live_in[(size_t)(i + 1) * (size_t)nnames + (size_t)v];
                if (target[i] >= 0)
                    for (int v = 0; v < nnames; v++)
                        out[v] |= live_in[(size_t)target[i] * (size_t)nnames + (size_t)v];
            }
            memcpy(row, out, (size_t)nnames * sizeof(bool));
            if (has_dst(in))
                row[name_index(names, &nnames, in->dst.name)] = false;
            Operand *use[MAX_PARAMS + 2];
            int nuse = used_operands(in, use);
            for (int u = 0; u < nuse; u++)
                if (is_name(*use[u]))
                    row[name_index(names, &nnames, use[u]->name)] = true;
            bool *in_row = &live_in[(size_t)i * (size_t)nnames];
            if (memcmp(in_row, row, (size_t)nnames * sizeof(bool)) != 0) {
                memcpy(in_row, row, (size_t)nnames * sizeof(bool));
                again = true;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        Instr *in = &fn->ins[i];
        if (!has_dst(in) || live_out[(size_t)i * (size_t)nnames + (size_t)name_index(names, &nnames, in->dst.name)])
            continue;
        if (in->op == IR_CALL) {
            /* The call may have side effects (print), so keep it and drop only the result. */
            Instr before = *in;
            in->dst = (Operand){OPD_NONE, 0, NULL};
            record("dead store elimination", &before, in);
            continue;
        }
        if (in->op == IR_BIN && (in->binop == TK_SLASH || in->binop == TK_PERCENT)) {
            /* A division can trap. Remove it only if the divisor is a constant that cannot trap. */
            if (!is_const(in->b) || in->b.value == 0 || in->b.value == -1)
                continue;
        }
        remove_instr("dead store elimination", in);
    }
    free(names);
    free(target);
    free(live_in);
    free(live_out);
    free(row);
    compact();
}

/* ----------------------------------------------------------------- driver */

void optimize(IrProgram *prog, OptLog *log)
{
    oplog = log;
    for (int f = 0; f < prog->count; f++) {
        fn = prog->funcs[f];
        for (int round = 0; round < 1000; round++) {
            changed = false;
            pass_propagate();
            pass_fold();
            pass_algebraic();
            pass_cse();
            pass_branch_fold();
            pass_unreachable();
            pass_unused_labels();
            pass_jump_to_next();
            pass_dead_stores();
            if (!changed)
                break;
        }
    }
}
