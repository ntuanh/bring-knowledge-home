/*
 * parser.c - stage 2: syntax analysis (hand-written recursive descent).
 *
 * Role: checks that the token sequence follows the grammar and builds the
 * abstract syntax tree (AST).
 * Input: the TokenList from lexer.c.
 * Output: an ND_PROGRAM node, or NULL plus one diagnostic. The parser stops
 *         at the first syntax error.
 * Called by: main.c (run_pipeline). The AST is consumed by sema.c and ir.c.
 *
 * There is one function per grammar rule. Operator precedence comes from the
 * call chain (lowest precedence first):
 *
 *   parse_assign -> parse_equality -> parse_relational -> parse_add
 *                -> parse_mul -> parse_unary -> parse_primary
 *
 * Left associativity comes from the `while` loops: `10 - 3 - 2` first builds
 * (10 - 3) and then makes it the left child of the second `-`.
 * Right associativity of `=` comes from parse_assign calling itself.
 */
#include "minic.h"

#include <setjmp.h>

static const Token *toks; /* token array, ends with TK_EOF */
static int pos;           /* index of the current token */
static DiagList *diags;
static jmp_buf on_error;  /* error recovery: jump back to parse() */
static int nesting;       /* current recursion depth of statements/expressions */

static const Token *peek(void) { return &toks[pos]; }
static const Token *previous(void) { return &toks[pos > 0 ? pos - 1 : 0]; }
static bool check(TokenKind kind) { return peek()->kind == kind; }

static const Token *advance(void)
{
    const Token *t = peek();
    if (t->kind != TK_EOF)
        pos++;
    return t;
}

static bool match(TokenKind kind)
{
    if (!check(kind))
        return false;
    advance();
    return true;
}

/* Text of the current token for error messages: 'return', 'x', end of file. */
static char *found_text(void)
{
    const Token *t = peek();
    if (t->kind == TK_EOF)
        return xstrdup("end of file");
    return xasprintf("'%.*s'", t->len, t->start);
}

static void fail_at(int line, int col, char *message)
{
    diag_add(diags, "parser", line, col, "%s", message);
    longjmp(on_error, 1);
}

/*
 * Requires a specific token. A missing token is reported just after the
 * previous token, because that is where the programmer has to insert it
 * (for a missing ';' this is the end of the statement, not the next line).
 */
static const Token *expect(TokenKind kind)
{
    if (check(kind))
        return advance();
    const Token *prev = previous();
    int line = pos > 0 ? prev->line : peek()->line;
    int col = pos > 0 ? prev->col + prev->len : peek()->col;
    fail_at(line, col, xasprintf("expected '%s' before %s", token_kind_text(kind), found_text()));
    return NULL;
}

static void enter(void)
{
    if (++nesting > MAX_NESTING)
        fail_at(peek()->line, peek()->col, xasprintf("nesting is too deep (limit %d)", MAX_NESTING));
}

static void leave(void) { nesting--; }

/* Marks the node as ending at the last consumed token. */
static Node *finish(Node *n)
{
    const Token *prev = previous();
    n->end_line = prev->line;
    n->end_col = prev->col + prev->len;
    return n;
}

static Node *node_at(NodeKind kind, const Token *t) { return node_new(kind, t->line, t->col); }

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_block(void);

/* Guards against expression chains so long that later recursive passes would overflow the stack. */
static Node *checked(Node *n)
{
    if (n->depth > 4 * MAX_NESTING)
        fail_at(n->line, n->col, xstrdup("expression is too long"));
    return n;
}

/* primary := NUMBER | IDENT | IDENT "(" args? ")" | "(" expr ")" */
static Node *parse_primary(void)
{
    const Token *t = peek();
    if (match(TK_NUMBER)) {
        Node *n = node_at(ND_NUMBER, t);
        n->value = t->value;
        return finish(n);
    }
    if (match(TK_IDENT)) {
        char *name = xstrndup(t->start, (size_t)t->len);
        if (match(TK_LPAREN)) {
            Node *call = node_at(ND_CALL, t);
            call->name = name;
            if (!check(TK_RPAREN)) {
                do {
                    node_add(call, parse_expr());
                } while (match(TK_COMMA));
            }
            expect(TK_RPAREN);
            return finish(call);
        }
        Node *var = node_at(ND_VAR, t);
        var->name = name;
        return finish(var);
    }
    if (match(TK_LPAREN)) {
        Node *inner = parse_expr();
        expect(TK_RPAREN);
        return inner; /* parentheses only group; they leave no node in the AST */
    }
    fail_at(t->line, t->col, xasprintf("expected an expression, found %s", found_text()));
    return NULL;
}

/* unary := ("-" | "+" | "!") unary | primary */
static Node *parse_unary(void)
{
    const Token *t = peek();
    if (match(TK_MINUS) || match(TK_PLUS) || match(TK_BANG)) {
        enter();
        Node *n = node_at(ND_UNARY, t);
        n->op = t->kind;
        node_add(n, parse_unary());
        leave();
        return finish(n);
    }
    return parse_primary();
}

static Node *binary(const Token *op, Node *lhs, Node *rhs)
{
    Node *n = node_new(ND_BINARY, lhs->line, lhs->col);
    n->op = op->kind;
    node_add(n, lhs);
    node_add(n, rhs);
    return checked(finish(n));
}

/* mul := unary (("*" | "/" | "%") unary)* */
static Node *parse_mul(void)
{
    Node *lhs = parse_unary();
    while (check(TK_STAR) || check(TK_SLASH) || check(TK_PERCENT)) {
        const Token *op = advance();
        lhs = binary(op, lhs, parse_unary());
    }
    return lhs;
}

/* add := mul (("+" | "-") mul)* */
static Node *parse_add(void)
{
    Node *lhs = parse_mul();
    while (check(TK_PLUS) || check(TK_MINUS)) {
        const Token *op = advance();
        lhs = binary(op, lhs, parse_mul());
    }
    return lhs;
}

/* relational := add (("<" | "<=" | ">" | ">=") add)* */
static Node *parse_relational(void)
{
    Node *lhs = parse_add();
    while (check(TK_LT) || check(TK_LE) || check(TK_GT) || check(TK_GE)) {
        const Token *op = advance();
        lhs = binary(op, lhs, parse_add());
    }
    return lhs;
}

/* equality := relational (("==" | "!=") relational)* */
static Node *parse_equality(void)
{
    Node *lhs = parse_relational();
    while (check(TK_EQ) || check(TK_NE)) {
        const Token *op = advance();
        lhs = binary(op, lhs, parse_relational());
    }
    return lhs;
}

/*
 * assign := equality ("=" assign)?
 * The left side is parsed as a general expression. Whether it is really a
 * variable is checked later by sema.c, which can give a better message.
 */
static Node *parse_assign(void)
{
    Node *lhs = parse_equality();
    if (match(TK_ASSIGN)) {
        enter();
        Node *n = node_new(ND_ASSIGN, lhs->line, lhs->col);
        node_add(n, lhs);
        node_add(n, parse_assign());
        leave();
        return finish(n);
    }
    return lhs;
}

/* expr := assign */
static Node *parse_expr(void)
{
    enter();
    Node *n = parse_assign();
    leave();
    return n;
}

/* stmt := declaration | return | if | while | block | expr ";" */
static Node *parse_stmt(void)
{
    const Token *t = peek();
    Node *n;
    enter();
    if (match(TK_KW_INT)) {
        /* "int" IDENT ("=" expr)? ";" */
        const Token *name = expect(TK_IDENT);
        n = node_at(ND_VAR_DECL, t);
        n->name = xstrndup(name->start, (size_t)name->len);
        if (match(TK_ASSIGN))
            node_add(n, parse_expr());
        expect(TK_SEMI);
    } else if (match(TK_KW_RETURN)) {
        /* "return" expr? ";" */
        n = node_at(ND_RETURN, t);
        if (!check(TK_SEMI))
            node_add(n, parse_expr());
        expect(TK_SEMI);
    } else if (match(TK_KW_IF)) {
        /* "if" "(" expr ")" stmt ("else" stmt)?  - an else binds to the nearest if */
        n = node_at(ND_IF, t);
        expect(TK_LPAREN);
        node_add(n, parse_expr());
        expect(TK_RPAREN);
        node_add(n, parse_stmt());
        if (match(TK_KW_ELSE))
            node_add(n, parse_stmt());
    } else if (match(TK_KW_WHILE)) {
        /* "while" "(" expr ")" stmt */
        n = node_at(ND_WHILE, t);
        expect(TK_LPAREN);
        node_add(n, parse_expr());
        expect(TK_RPAREN);
        node_add(n, parse_stmt());
    } else if (check(TK_LBRACE)) {
        leave();
        return parse_block();
    } else if (check(TK_KW_VOID)) {
        fail_at(t->line, t->col, xstrdup("variables cannot have type 'void'"));
        return NULL;
    } else {
        n = node_at(ND_EXPR_STMT, t);
        node_add(n, parse_expr());
        expect(TK_SEMI);
    }
    leave();
    return finish(n);
}

/* block := "{" stmt* "}" */
static Node *parse_block(void)
{
    enter();
    Node *block = node_at(ND_BLOCK, expect(TK_LBRACE));
    while (!check(TK_RBRACE)) {
        if (check(TK_EOF))
            expect(TK_RBRACE); /* reports the missing '}' */
        node_add(block, parse_stmt());
    }
    expect(TK_RBRACE);
    leave();
    return finish(block);
}

/* function := ("int" | "void") IDENT "(" params? ")" block */
static Node *parse_function(void)
{
    const Token *t = peek();
    if (!match(TK_KW_INT) && !match(TK_KW_VOID))
        fail_at(t->line, t->col, xasprintf("expected 'int' or 'void' at the start of a function, found %s", found_text()));
    const Token *name = expect(TK_IDENT);
    Node *fn = node_at(ND_FUNCTION, t);
    fn->ret_type = (t->kind == TK_KW_VOID) ? TY_VOID : TY_INT;
    fn->name = xstrndup(name->start, (size_t)name->len);
    expect(TK_LPAREN);
    /* params := "int" IDENT ("," "int" IDENT)*   ("(void)" means no parameters) */
    if (check(TK_KW_VOID) && toks[pos + 1].kind == TK_RPAREN) {
        advance();
    } else if (!check(TK_RPAREN)) {
        do {
            const Token *pt = expect(TK_KW_INT);
            const Token *pname = expect(TK_IDENT);
            if (fn->nkids == MAX_PARAMS)
                fail_at(pt->line, pt->col, xasprintf("function '%s' has more than %d parameters", fn->name, MAX_PARAMS));
            Node *param = node_at(ND_PARAM, pt);
            param->name = xstrndup(pname->start, (size_t)pname->len);
            node_add(fn, finish(param));
        } while (match(TK_COMMA));
    }
    expect(TK_RPAREN);
    node_add(fn, parse_block());
    return finish(fn);
}

/* program := function* */
Node *parse(const TokenList *tokens, DiagList *diag_list)
{
    toks = tokens->items;
    pos = 0;
    nesting = 0;
    diags = diag_list;
    if (setjmp(on_error))
        return NULL;
    Node *program = node_new(ND_PROGRAM, 1, 1);
    while (!check(TK_EOF))
        node_add(program, parse_function());
    return finish(program);
}
