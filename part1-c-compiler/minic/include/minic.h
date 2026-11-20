/*
 * minic.h - shared declarations for every stage of the mini C compiler.
 *
 * Pipeline (each arrow is one source file):
 *
 *   source text --lexer.c--> tokens --parser.c--> AST --sema.c--> checked AST
 *        --ir.c--> three-address IR --opt.c--> optimized IR --codegen.c--> x86-64 assembly
 *
 * main.c drives the stages; json.c serializes every stage for the visualizer;
 * ast.c and util.c hold helpers. minic stops at assembly text: the object file
 * is produced by `as` and the ELF executable by `gcc`/`ld`.
 */
#ifndef MINIC_H
#define MINIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ util.c */

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Growable string, used for IR text, assembly text and messages. */
typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* One error message with its source position (1-based line and column). */
typedef struct {
    const char *stage; /* "lexer", "parser" or "sema" */
    int line, col;
    char *message;
} Diag;

typedef struct {
    Diag *items;
    int count, cap;
} DiagList;

void diag_add(DiagList *list, const char *stage, int line, int col, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* ----------------------------------------------------------------- lexer.c */

typedef enum {
    TK_NUMBER, TK_IDENT,
    TK_KW_INT, TK_KW_VOID, TK_KW_RETURN, TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_BANG,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_COMMA, TK_SEMI,
    TK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start; /* points into the source buffer */
    int len;
    int32_t value;     /* for TK_NUMBER */
    int line, col;
} Token;

typedef struct {
    Token *items;
    int count, cap;
} TokenList;

/* Returns false (and adds one diagnostic) on the first lexical error. */
bool lex(const char *src, TokenList *out, DiagList *diags);
const char *token_kind_name(TokenKind kind);   /* "PLUS", "KW_INT", ... */
const char *token_kind_text(TokenKind kind);   /* "+", "int", ... */
void token_display(const Token *t, StrBuf *sb); /* "NUMBER(10)", "IDENT(a)", "PLUS" */

/* ------------------------------------------------------------------- ast.c */

typedef enum { TY_INT, TY_VOID } Type;

typedef enum {
    ND_PROGRAM,   /* kids: functions */
    ND_FUNCTION,  /* name, ret_type; kids: params..., then the body block */
    ND_PARAM,     /* name */
    ND_BLOCK,     /* kids: statements */
    ND_VAR_DECL,  /* name; kids: optional initializer */
    ND_RETURN,    /* kids: optional value */
    ND_IF,        /* kids: condition, then, optional else */
    ND_WHILE,     /* kids: condition, body */
    ND_EXPR_STMT, /* kids: expression */
    ND_ASSIGN,    /* kids: target, value */
    ND_BINARY,    /* op; kids: left, right */
    ND_UNARY,     /* op; kids: operand */
    ND_NUMBER,    /* value */
    ND_VAR,       /* name */
    ND_CALL       /* name; kids: arguments */
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    int line, col;         /* first character of the construct */
    int end_line, end_col; /* one past its last character */
    TokenKind op;          /* ND_BINARY, ND_UNARY */
    int32_t value;         /* ND_NUMBER */
    char *name;            /* identifiers */
    Type ret_type;         /* ND_FUNCTION */
    Node **kids;
    int nkids, kcap;
    int depth;             /* height of the subtree, used to cap nesting */
    /* Filled in by sema.c: */
    Type type;             /* type of an expression */
    const char *ir_name;   /* unique variable name used in the IR (a, a.1, ...) */
};

Node *node_new(NodeKind kind, int line, int col);
void node_add(Node *parent, Node *kid);
const char *node_kind_name(NodeKind kind);
void node_label(const Node *n, StrBuf *sb);            /* e.g. "Binary(+)" */
void ast_print(const Node *n, int indent, StrBuf *sb); /* indented tree */

/* ---------------------------------------------------------------- parser.c */

#define MAX_PARAMS 6
#define MAX_NESTING 200

/* Returns the ND_PROGRAM node, or NULL after adding one diagnostic. */
Node *parse(const TokenList *tokens, DiagList *diags);

/* ------------------------------------------------------------------ sema.c */

typedef enum { SYM_VAR, SYM_PARAM, SYM_FUNC } SymKind;

typedef struct {
    char *name;
    SymKind kind;
    Type type;           /* variable type, or function return type */
    int nparams;         /* SYM_FUNC */
    bool builtin;        /* the built-in print() */
    int line, col;
    const char *ir_name; /* variables only */
} Symbol;

typedef struct Scope Scope;
struct Scope {
    int id, parent_id;   /* parent_id is -1 for the global scope */
    const char *kind;    /* "global", "function" or "block" */
    char *label;         /* e.g. "function main", "block at line 4" */
    Scope *parent;
    Symbol **syms;
    int nsyms, cap;
};

typedef struct {
    Scope **scopes;      /* every scope ever opened, in creation order */
    int count, cap;
} SymbolTable;

/* Checks the whole program and reports every error. Returns true if clean. */
bool analyze(Node *program, SymbolTable *table, DiagList *diags);

/* -------------------------------------------------------------------- ir.c */

typedef enum { OPD_NONE, OPD_CONST, OPD_VAR, OPD_TEMP } OperandKind;

typedef struct {
    OperandKind kind;
    int32_t value;    /* OPD_CONST */
    const char *name; /* OPD_VAR ("a", "a.1") and OPD_TEMP ("t3") */
} Operand;

typedef enum {
    IR_NOP,    /* deleted instruction (removed when the optimizer compacts) */
    IR_COPY,   /* dst = a */
    IR_BIN,    /* dst = a binop b */
    IR_UN,     /* dst = unop a */
    IR_LABEL,  /* L<label>: */
    IR_JUMP,   /* jump L<label> */
    IR_BRZ,    /* branch_if_zero a, L<label> */
    IR_CALL,   /* [dst =] call fname(args...) */
    IR_RET     /* return [a] */
} IrOp;

typedef struct {
    IrOp op;
    TokenKind binop;           /* operator for IR_BIN and IR_UN */
    Operand dst, a, b;
    int label;
    const char *fname;
    Operand args[MAX_PARAMS];
    int nargs;
    int line;                  /* source line this instruction came from */
} Instr;

typedef struct {
    const char *name;
    Type ret_type;
    const char *params[MAX_PARAMS];
    int nparams;
    Instr *ins;
    int count, cap;
    int ntemps;
} IrFunc;

typedef struct {
    IrFunc **funcs;
    int count;
} IrProgram;

IrProgram *ir_generate(const Node *program);
IrProgram *ir_clone(const IrProgram *prog);
bool operand_eq(Operand x, Operand y);
void operand_text(Operand o, StrBuf *sb);
void instr_text(const Instr *in, StrBuf *sb);       /* one line, no newline */
void ir_print(const IrProgram *prog, StrBuf *sb);

/* ------------------------------------------------------------------- opt.c */

typedef struct {
    const char *pass;   /* e.g. "constant folding" */
    const char *func;   /* function the change was made in */
    char *before;       /* instruction text before the change */
    char *after;        /* instruction text after, or "(removed)" */
} OptEntry;

typedef struct {
    OptEntry *items;
    int count, cap;
} OptLog;

/* Optimizes in place and records every change in the log. */
void optimize(IrProgram *prog, OptLog *log);

/* --------------------------------------------------------------- codegen.c */

void codegen(const IrProgram *prog, StrBuf *out);

/* ------------------------------------------------------------------ json.c */

typedef struct {
    const char *filename;
    int opt_level;
    TokenList tokens;
    bool have_tokens;
    Node *ast;
    SymbolTable symbols;
    bool have_symbols;
    DiagList diags;
    IrProgram *ir;
    IrProgram *ir_opt;
    OptLog opt_log;
    StrBuf asm_text;
    bool have_asm;
    const char *failed_stage; /* NULL when every stage succeeded */
} Compilation;

void json_emit(FILE *out, const Compilation *c);

#endif
