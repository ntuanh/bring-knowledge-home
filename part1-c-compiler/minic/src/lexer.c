/*
 * lexer.c - stage 1: lexical analysis.
 *
 * Role: turns the raw character stream into tokens, dropping whitespace and
 * comments, so the parser never has to look at individual characters.
 * Input: the source text (a NUL-terminated string).
 * Output: a TokenList ending in TK_EOF; each token records its kind, its
 *         text, and its 1-based line and column.
 * Called by: main.c (run_pipeline). The tokens are consumed by parser.c.
 *
 * The lexer stops at the first error (unknown character, unterminated
 * comment, malformed or too large number).
 */
#include "minic.h"

#include <ctype.h>
#include <string.h>

static const struct {
    const char *name; /* name shown to the user */
    const char *text; /* fixed spelling, or a description */
} kind_info[] = {
    [TK_NUMBER] = {"NUMBER", "number"},   [TK_IDENT] = {"IDENT", "identifier"},
    [TK_KW_INT] = {"KW_INT", "int"},      [TK_KW_VOID] = {"KW_VOID", "void"},
    [TK_KW_RETURN] = {"KW_RETURN", "return"}, [TK_KW_IF] = {"KW_IF", "if"},
    [TK_KW_ELSE] = {"KW_ELSE", "else"},   [TK_KW_WHILE] = {"KW_WHILE", "while"},
    [TK_PLUS] = {"PLUS", "+"},            [TK_MINUS] = {"MINUS", "-"},
    [TK_STAR] = {"STAR", "*"},            [TK_SLASH] = {"SLASH", "/"},
    [TK_PERCENT] = {"PERCENT", "%"},      [TK_ASSIGN] = {"ASSIGN", "="},
    [TK_EQ] = {"EQ", "=="},               [TK_NE] = {"NE", "!="},
    [TK_LT] = {"LT", "<"},                [TK_LE] = {"LE", "<="},
    [TK_GT] = {"GT", ">"},                [TK_GE] = {"GE", ">="},
    [TK_BANG] = {"BANG", "!"},            [TK_LPAREN] = {"LPAREN", "("},
    [TK_RPAREN] = {"RPAREN", ")"},        [TK_LBRACE] = {"LBRACE", "{"},
    [TK_RBRACE] = {"RBRACE", "}"},        [TK_COMMA] = {"COMMA", ","},
    [TK_SEMI] = {"SEMI", ";"},            [TK_EOF] = {"EOF", "end of file"},
};

const char *token_kind_name(TokenKind kind) { return kind_info[kind].name; }
const char *token_kind_text(TokenKind kind) { return kind_info[kind].text; }

void token_display(const Token *t, StrBuf *sb)
{
    sb_puts(sb, token_kind_name(t->kind));
    if (t->kind == TK_NUMBER || t->kind == TK_IDENT)
        sb_printf(sb, "(%.*s)", t->len, t->start);
}

static const struct {
    const char *word;
    TokenKind kind;
} keywords[] = {
    {"int", TK_KW_INT}, {"void", TK_KW_VOID}, {"return", TK_KW_RETURN},
    {"if", TK_KW_IF},   {"else", TK_KW_ELSE}, {"while", TK_KW_WHILE},
};

static void push_token(TokenList *list, TokenKind kind, const char *start, int len, int line, int col, int32_t value)
{
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 64;
        list->items = xrealloc(list->items, (size_t)list->cap * sizeof(Token));
    }
    list->items[list->count++] = (Token){kind, start, len, value, line, col};
}

bool lex(const char *src, TokenList *out, DiagList *diags)
{
    const char *p = src;
    int line = 1, col = 1;

    for (;;) {
        /* Skip whitespace and comments, keeping line/col up to date. */
        if (*p == '\n') {
            p++, line++, col = 1;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++, col++;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++, col++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            int start_line = line, start_col = col;
            p += 2, col += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n')
                    line++, col = 1;
                else
                    col++;
                p++;
            }
            if (!*p) {
                diag_add(diags, "lexer", start_line, start_col, "unterminated /* comment");
                return false;
            }
            p += 2, col += 2;
            continue;
        }

        const char *start = p;
        int start_col = col;

        if (*p == '\0') {
            push_token(out, TK_EOF, p, 0, line, col, 0);
            return true;
        }

        if (isdigit((unsigned char)*p)) {
            int64_t value = 0;
            bool too_large = false;
            while (isdigit((unsigned char)*p)) {
                value = value * 10 + (*p - '0');
                if (value > INT32_MAX)
                    too_large = true, value = 0;
                p++;
            }
            if (isalpha((unsigned char)*p) || *p == '_') {
                diag_add(diags, "lexer", line, start_col, "invalid number: letters directly after digits");
                return false;
            }
            if (too_large) {
                diag_add(diags, "lexer", line, start_col, "integer literal is too large for a 32-bit int");
                return false;
            }
            push_token(out, TK_NUMBER, start, (int)(p - start), line, start_col, (int32_t)value);
            col += (int)(p - start);
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            int len = (int)(p - start);
            TokenKind kind = TK_IDENT;
            for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
                if ((int)strlen(keywords[i].word) == len && strncmp(start, keywords[i].word, (size_t)len) == 0)
                    kind = keywords[i].kind;
            }
            push_token(out, kind, start, len, line, start_col, 0);
            col += len;
            continue;
        }

        /* Operators and punctuation. Two-character operators are tried first. */
        TokenKind kind;
        int len = 1;
        switch (*p) {
        case '+': kind = TK_PLUS; break;
        case '-': kind = TK_MINUS; break;
        case '*': kind = TK_STAR; break;
        case '/': kind = TK_SLASH; break;
        case '%': kind = TK_PERCENT; break;
        case '(': kind = TK_LPAREN; break;
        case ')': kind = TK_RPAREN; break;
        case '{': kind = TK_LBRACE; break;
        case '}': kind = TK_RBRACE; break;
        case ',': kind = TK_COMMA; break;
        case ';': kind = TK_SEMI; break;
        case '=': kind = (p[1] == '=') ? (len = 2, TK_EQ) : TK_ASSIGN; break;
        case '!': kind = (p[1] == '=') ? (len = 2, TK_NE) : TK_BANG; break;
        case '<': kind = (p[1] == '=') ? (len = 2, TK_LE) : TK_LT; break;
        case '>': kind = (p[1] == '=') ? (len = 2, TK_GE) : TK_GT; break;
        default:
            if (isprint((unsigned char)*p))
                diag_add(diags, "lexer", line, col, "unexpected character '%c'", *p);
            else
                diag_add(diags, "lexer", line, col, "unexpected byte 0x%02x", (unsigned char)*p);
            return false;
        }
        push_token(out, kind, start, len, line, start_col, 0);
        p += len, col += len;
    }
}
