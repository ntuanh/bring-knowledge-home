/*
 * sema.c - stage 3: semantic analysis.
 *
 * Role: checks the rules the grammar cannot express (names, scopes, types)
 * and annotates the AST for the IR generator.
 * Input: the AST from parser.c.
 * Output: a SymbolTable (one Scope per function and per block), diagnostics
 *         for *every* semantic error, and two annotations on AST nodes:
 *         `type` (int or void) and `ir_name` (unique variable name).
 * Called by: main.c (run_pipeline). ir.c relies on the annotations.
 *
 * Function names are collected in a first pass, so a function may be called
 * before its definition (real C would need a prototype). Variables must be
 * declared before use. Inner scopes may shadow outer variables; each
 * declaration gets a unique IR name (a, a.1, a.2, ...) so later stages do not
 * need scopes at all.
 */
#include "minic.h"

#include <ctype.h>
#include <string.h>

static SymbolTable *table;
static DiagList *diags;
static Scope *current;        /* innermost open scope */
static const Node *function;  /* function being checked */

/* How many times each variable name was declared in the current function. */
static struct NameUse {
    const char *name;
    int count;
} *uses;
static int nuses, uses_cap;

static void error(const Node *n, const char *msg) { diag_add(diags, "sema", n->line, n->col, "%s", msg); }

static Scope *open_scope(const char *kind, char *label)
{
    Scope *s = xcalloc(1, sizeof(Scope));
    s->id = table->count;
    s->parent = current;
    s->parent_id = current ? current->id : -1;
    s->kind = kind;
    s->label = label;
    if (table->count == table->cap) {
        table->cap = table->cap ? table->cap * 2 : 8;
        table->scopes = xrealloc(table->scopes, (size_t)table->cap * sizeof(Scope *));
    }
    table->scopes[table->count++] = s;
    current = s;
    return s;
}

static void close_scope(void) { current = current->parent; }

static Symbol *lookup_in(const Scope *s, const char *name)
{
    for (int i = 0; i < s->nsyms; i++)
        if (strcmp(s->syms[i]->name, name) == 0)
            return s->syms[i];
    return NULL;
}

/* Searches the innermost scope first, then each enclosing scope. */
static Symbol *lookup(const char *name)
{
    for (const Scope *s = current; s; s = s->parent) {
        Symbol *sym = lookup_in(s, name);
        if (sym)
            return sym;
    }
    return NULL;
}

static Symbol *add_symbol(Scope *s, const char *name, SymKind kind, Type type, int line, int col)
{
    Symbol *sym = xcalloc(1, sizeof(Symbol));
    sym->name = xstrdup(name);
    sym->kind = kind;
    sym->type = type;
    sym->line = line;
    sym->col = col;
    if (s->nsyms == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->syms = xrealloc(s->syms, (size_t)s->cap * sizeof(Symbol *));
    }
    s->syms[s->nsyms++] = sym;
    return sym;
}

/* A user variable called t1, t2, ... would collide with IR temporaries. */
static bool looks_like_temp(const char *name)
{
    if (name[0] != 't' || !name[1])
        return false;
    for (const char *p = name + 1; *p; p++)
        if (!isdigit((unsigned char)*p))
            return false;
    return true;
}

/* First declaration of `a` in a function is "a", later ones "a.1", "a.2", ... */
static const char *unique_name(const char *name)
{
    struct NameUse *use = NULL;
    for (int i = 0; i < nuses; i++)
        if (strcmp(uses[i].name, name) == 0)
            use = &uses[i];
    if (!use) {
        if (nuses == uses_cap) {
            uses_cap = uses_cap ? uses_cap * 2 : 16;
            uses = xrealloc(uses, (size_t)uses_cap * sizeof *uses);
        }
        use = &uses[nuses++];
        use->name = name;
        use->count = looks_like_temp(name) ? 1 : 0;
    }
    int n = use->count++;
    return n == 0 ? name : xasprintf("%s.%d", name, n);
}

/* Declares a variable or parameter in the current scope. */
static void declare_variable(Node *n, SymKind kind)
{
    Symbol *prev = lookup_in(current, n->name);
    if (prev) {
        diag_add(diags, "sema", n->line, n->col, "redeclaration of '%s' in the same scope (first declared at %d:%d)",
                 n->name, prev->line, prev->col);
        n->ir_name = prev->ir_name ? prev->ir_name : n->name;
        return;
    }
    Symbol *sym = add_symbol(current, n->name, kind, TY_INT, n->line, n->col);
    sym->ir_name = unique_name(sym->name);
    n->ir_name = sym->ir_name;
}

static Type check_expr(Node *n);

/* Checks an expression whose value is needed, so it must not be void. */
static void check_value(Node *n)
{
    if (check_expr(n) == TY_VOID) {
        if (n->kind == ND_CALL)
            diag_add(diags, "sema", n->line, n->col,
                     "void value used where an int is required (function '%s' returns void)", n->name);
        else
            error(n, "void value used where an int is required");
    }
}

static Type check_expr(Node *n)
{
    n->type = TY_INT;
    switch (n->kind) {
    case ND_NUMBER:
        break;
    case ND_VAR: {
        Symbol *sym = lookup(n->name);
        n->ir_name = n->name;
        if (!sym)
            diag_add(diags, "sema", n->line, n->col, "use of undeclared variable '%s'", n->name);
        else if (sym->kind == SYM_FUNC)
            diag_add(diags, "sema", n->line, n->col, "'%s' is a function, not a variable", n->name);
        else
            n->ir_name = sym->ir_name;
        break;
    }
    case ND_ASSIGN:
        if (n->kids[0]->kind != ND_VAR)
            error(n->kids[0], "left side of '=' must be a variable");
        check_expr(n->kids[0]);
        check_value(n->kids[1]);
        break;
    case ND_BINARY:
        check_value(n->kids[0]);
        check_value(n->kids[1]);
        break;
    case ND_UNARY:
        check_value(n->kids[0]);
        break;
    case ND_CALL: {
        Symbol *sym = lookup(n->name);
        if (!sym)
            diag_add(diags, "sema", n->line, n->col, "call to undefined function '%s'", n->name);
        else if (sym->kind != SYM_FUNC)
            diag_add(diags, "sema", n->line, n->col, "'%s' is a variable, not a function", n->name);
        else {
            if (sym->nparams != n->nkids)
                diag_add(diags, "sema", n->line, n->col, "function '%s' expects %d argument%s but %d %s given",
                         n->name, sym->nparams, sym->nparams == 1 ? "" : "s", n->nkids, n->nkids == 1 ? "was" : "were");
            n->type = sym->type;
        }
        for (int i = 0; i < n->nkids; i++)
            check_value(n->kids[i]);
        break;
    }
    default:
        break;
    }
    return n->type;
}

static void check_stmt(Node *n);

static void check_block_items(Node *block)
{
    for (int i = 0; i < block->nkids; i++)
        check_stmt(block->kids[i]);
}

static void check_stmt(Node *n)
{
    switch (n->kind) {
    case ND_VAR_DECL:
        /* The initializer is checked first: in `int x = x + 1;` the x on the right is not this x. */
        if (n->nkids)
            check_value(n->kids[0]);
        declare_variable(n, SYM_VAR);
        break;
    case ND_RETURN:
        if (function->ret_type == TY_VOID) {
            if (n->nkids) {
                diag_add(diags, "sema", n->line, n->col, "return with a value in void function '%s'", function->name);
                check_expr(n->kids[0]);
            }
        } else if (!n->nkids) {
            diag_add(diags, "sema", n->line, n->col, "return without a value in int function '%s'", function->name);
        } else {
            check_value(n->kids[0]);
        }
        break;
    case ND_IF:
        check_value(n->kids[0]);
        check_stmt(n->kids[1]);
        if (n->nkids > 2)
            check_stmt(n->kids[2]);
        break;
    case ND_WHILE:
        check_value(n->kids[0]);
        check_stmt(n->kids[1]);
        break;
    case ND_BLOCK:
        open_scope("block", xasprintf("block at line %d", n->line));
        check_block_items(n);
        close_scope();
        break;
    case ND_EXPR_STMT:
        check_expr(n->kids[0]); /* a void call is fine here: its value is not used */
        break;
    default:
        break;
    }
}

bool analyze(Node *program, SymbolTable *out, DiagList *diag_list)
{
    table = out;
    diags = diag_list;
    current = NULL;
    int errors_before = diags->count;

    Scope *global = open_scope("global", xstrdup("global"));
    Symbol *print = add_symbol(global, "print", SYM_FUNC, TY_VOID, 0, 0);
    print->nparams = 1;
    print->builtin = true;

    /* Pass 1: declare every function, so calls can refer to later definitions. */
    bool *duplicate = xcalloc((size_t)program->nkids, sizeof(bool));
    for (int i = 0; i < program->nkids; i++) {
        Node *fn = program->kids[i];
        Symbol *prev = lookup_in(global, fn->name);
        if (prev) {
            duplicate[i] = true;
            if (prev->builtin)
                diag_add(diags, "sema", fn->line, fn->col, "duplicate function '%s' (it is a built-in function)", fn->name);
            else
                diag_add(diags, "sema", fn->line, fn->col, "duplicate function '%s' (first defined at %d:%d)",
                         fn->name, prev->line, prev->col);
            continue;
        }
        Symbol *sym = add_symbol(global, fn->name, SYM_FUNC, fn->ret_type, fn->line, fn->col);
        sym->nparams = fn->nkids - 1; /* the last kid is the body */
    }

    Symbol *main_sym = lookup_in(global, "main");
    if (!main_sym)
        diag_add(diags, "sema", 1, 1, "program has no 'main' function");
    else if (main_sym->type != TY_INT || main_sym->nparams != 0)
        diag_add(diags, "sema", main_sym->line, main_sym->col, "'main' must return int and take no parameters");

    /* Pass 2: check every function body (duplicates too, to report their errors). */
    for (int i = 0; i < program->nkids; i++) {
        Node *fn = program->kids[i];
        function = fn;
        nuses = 0;
        open_scope("function", xasprintf("function %s%s", fn->name, duplicate[i] ? " (duplicate)" : ""));
        for (int p = 0; p < fn->nkids - 1; p++)
            declare_variable(fn->kids[p], SYM_PARAM);
        /* Parameters and the outermost block share one scope, as in C. */
        check_block_items(fn->kids[fn->nkids - 1]);
        close_scope();
    }
    close_scope();
    return diags->count == errors_before;
}
