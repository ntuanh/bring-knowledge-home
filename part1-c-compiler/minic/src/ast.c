/*
 * ast.c - construction and printing of the abstract syntax tree.
 *
 * Role: helper for stage 2. Defines how AST nodes are created and how they
 * are shown to a human.
 * Input: node kinds and source positions (from parser.c).
 * Output: Node objects; text for `--emit=ast` and labels for the JSON output.
 * Called by: parser.c (node_new, node_add), main.c (ast_print), json.c (node_label).
 */
#include "minic.h"

Node *node_new(NodeKind kind, int line, int col)
{
    Node *n = xcalloc(1, sizeof(Node));
    n->kind = kind;
    n->line = n->end_line = line;
    n->col = n->end_col = col;
    n->depth = 1;
    return n;
}

void node_add(Node *parent, Node *kid)
{
    if (parent->nkids == parent->kcap) {
        parent->kcap = parent->kcap ? parent->kcap * 2 : 4;
        parent->kids = xrealloc(parent->kids, (size_t)parent->kcap * sizeof(Node *));
    }
    parent->kids[parent->nkids++] = kid;
    if (kid->depth + 1 > parent->depth)
        parent->depth = kid->depth + 1;
}

const char *node_kind_name(NodeKind kind)
{
    switch (kind) {
    case ND_PROGRAM:   return "Program";
    case ND_FUNCTION:  return "Function";
    case ND_PARAM:     return "Param";
    case ND_BLOCK:     return "Block";
    case ND_VAR_DECL:  return "VarDecl";
    case ND_RETURN:    return "Return";
    case ND_IF:        return "If";
    case ND_WHILE:     return "While";
    case ND_EXPR_STMT: return "ExprStmt";
    case ND_ASSIGN:    return "Assign";
    case ND_BINARY:    return "Binary";
    case ND_UNARY:     return "Unary";
    case ND_NUMBER:    return "Number";
    case ND_VAR:       return "Var";
    case ND_CALL:      return "Call";
    }
    return "?";
}

void node_label(const Node *n, StrBuf *sb)
{
    sb_puts(sb, node_kind_name(n->kind));
    switch (n->kind) {
    case ND_FUNCTION:
        sb_printf(sb, "(%s) -> %s", n->name, n->ret_type == TY_VOID ? "void" : "int");
        break;
    case ND_PARAM:
    case ND_VAR_DECL:
    case ND_VAR:
    case ND_CALL:
        sb_printf(sb, "(%s)", n->name);
        break;
    case ND_BINARY:
    case ND_UNARY:
        sb_printf(sb, "(%s)", token_kind_text(n->op));
        break;
    case ND_NUMBER:
        sb_printf(sb, "(%d)", n->value);
        break;
    default:
        break;
    }
}

void ast_print(const Node *n, int indent, StrBuf *sb)
{
    for (int i = 0; i < indent; i++)
        sb_puts(sb, "  ");
    node_label(n, sb);
    sb_putc(sb, '\n');
    for (int i = 0; i < n->nkids; i++)
        ast_print(n->kids[i], indent + 1, sb);
}
