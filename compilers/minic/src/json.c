/*
 * json.c - serializes every pipeline stage as one JSON object.
 *
 * Role: not a compilation stage. It is the interface between minic and the
 * web visualizer (`minic --json`).
 * Input: a Compilation (the results of all stages that ran).
 * Output: JSON text on a FILE. Stages that did not run are `null`.
 * Called by: main.c.
 *
 * Top-level keys: file, opt, tokens, ast, symbols, diagnostics, ir, ir_opt,
 * opt_log, asm, failed_stage.
 */
#include "minic.h"

static FILE *f;

static void str(const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\t': fputs("\\t", f); break;
        case '\r': fputs("\\r", f); break;
        default:
            if (c < 0x20 || c >= 0x7f)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
        }
    }
    fputc('"', f);
}

static void key(const char *name)
{
    str(name);
    fputc(':', f);
}

static void operand(Operand o)
{
    StrBuf sb;
    sb_init(&sb);
    operand_text(o, &sb);
    str(sb.data);
}

static void emit_tokens(const TokenList *tokens)
{
    fputc('[', f);
    for (int i = 0; i < tokens->count; i++) {
        const Token *t = &tokens->items[i];
        StrBuf text, display;
        sb_init(&text);
        sb_init(&display);
        sb_printf(&text, "%.*s", t->len, t->start);
        token_display(t, &display);
        fprintf(f, "%s{", i ? "," : "");
        key("kind"), str(token_kind_name(t->kind));
        fputc(',', f), key("text"), str(text.data);
        fputc(',', f), key("display"), str(display.data);
        fprintf(f, ",\"line\":%d,\"col\":%d,\"len\":%d}", t->line, t->col, t->len);
    }
    fputc(']', f);
}

static void emit_node(const Node *n)
{
    StrBuf label;
    sb_init(&label);
    node_label(n, &label);
    fputc('{', f);
    key("kind"), str(node_kind_name(n->kind));
    fputc(',', f), key("label"), str(label.data);
    if (n->name)
        fputc(',', f), key("name"), str(n->name);
    if (n->kind == ND_BINARY || n->kind == ND_UNARY)
        fputc(',', f), key("op"), str(token_kind_text(n->op));
    if (n->kind == ND_NUMBER)
        fprintf(f, ",\"value\":%d", n->value);
    if (n->kind == ND_FUNCTION)
        fputc(',', f), key("returns"), str(n->ret_type == TY_VOID ? "void" : "int");
    if (n->ir_name)
        fputc(',', f), key("ir_name"), str(n->ir_name);
    fprintf(f, ",\"line\":%d,\"col\":%d,\"end_line\":%d,\"end_col\":%d,\"children\":[", n->line, n->col, n->end_line,
            n->end_col);
    for (int i = 0; i < n->nkids; i++) {
        if (i)
            fputc(',', f);
        emit_node(n->kids[i]);
    }
    fputs("]}", f);
}

static void emit_symbols(const SymbolTable *table)
{
    fputc('[', f);
    for (int i = 0; i < table->count; i++) {
        const Scope *s = table->scopes[i];
        fprintf(f, "%s{\"id\":%d,\"parent\":%d,", i ? "," : "", s->id, s->parent_id);
        key("kind"), str(s->kind);
        fputc(',', f), key("label"), str(s->label);
        fputs(",\"symbols\":[", f);
        for (int k = 0; k < s->nsyms; k++) {
            const Symbol *sym = s->syms[k];
            fprintf(f, "%s{", k ? "," : "");
            key("name"), str(sym->name);
            fputc(',', f), key("kind");
            str(sym->kind == SYM_FUNC ? (sym->builtin ? "built-in function" : "function")
                                      : sym->kind == SYM_PARAM ? "parameter" : "variable");
            fputc(',', f), key("type"), str(sym->type == TY_VOID ? "void" : "int");
            if (sym->kind == SYM_FUNC)
                fprintf(f, ",\"nparams\":%d", sym->nparams);
            else
                fputc(',', f), key("ir_name"), str(sym->ir_name);
            fprintf(f, ",\"line\":%d,\"col\":%d}", sym->line, sym->col);
        }
        fputs("]}", f);
    }
    fputc(']', f);
}

static void emit_diags(const DiagList *diags)
{
    fputc('[', f);
    for (int i = 0; i < diags->count; i++) {
        const Diag *d = &diags->items[i];
        fprintf(f, "%s{", i ? "," : "");
        key("stage"), str(d->stage);
        fprintf(f, ",\"line\":%d,\"col\":%d,", d->line, d->col);
        key("message"), str(d->message);
        fputc('}', f);
    }
    fputc(']', f);
}

static const char *ir_op_name(IrOp op)
{
    switch (op) {
    case IR_COPY:  return "copy";
    case IR_BIN:   return "binary";
    case IR_UN:    return "unary";
    case IR_LABEL: return "label";
    case IR_JUMP:  return "jump";
    case IR_BRZ:   return "branch_if_zero";
    case IR_CALL:  return "call";
    case IR_RET:   return "return";
    default:       return "nop";
    }
}

static void emit_ir(const IrProgram *prog)
{
    fputc('[', f);
    for (int i = 0; i < prog->count; i++) {
        const IrFunc *fn = prog->funcs[i];
        fprintf(f, "%s{", i ? "," : "");
        key("name"), str(fn->name);
        fputc(',', f), key("returns"), str(fn->ret_type == TY_VOID ? "void" : "int");
        fputs(",\"params\":[", f);
        for (int p = 0; p < fn->nparams; p++) {
            if (p)
                fputc(',', f);
            str(fn->params[p]);
        }
        fputs("],\"instrs\":[", f);
        for (int k = 0; k < fn->count; k++) {
            const Instr *in = &fn->ins[k];
            StrBuf text;
            sb_init(&text);
            instr_text(in, &text);
            fprintf(f, "%s{", k ? "," : "");
            key("op"), str(ir_op_name(in->op));
            fputc(',', f), key("text"), str(text.data);
            fprintf(f, ",\"line\":%d,", in->line);
            /* Columns for the table view: dst = arg1 operator arg2 */
            key("dst"), operand(in->dst);
            fputc(',', f), key("arg1");
            if (in->op == IR_CALL) {
                StrBuf args;
                sb_init(&args);
                sb_printf(&args, "%s(", in->fname);
                for (int a = 0; a < in->nargs; a++) {
                    if (a)
                        sb_puts(&args, ", ");
                    operand_text(in->args[a], &args);
                }
                sb_putc(&args, ')');
                str(args.data);
            } else if (in->op == IR_LABEL || in->op == IR_JUMP) {
                str(xasprintf("L%d", in->label));
            } else {
                operand(in->a);
            }
            fputc(',', f), key("operator");
            str(in->op == IR_BIN || in->op == IR_UN ? token_kind_text(in->binop) : "");
            fputc(',', f), key("arg2");
            if (in->op == IR_BRZ)
                str(xasprintf("L%d", in->label));
            else
                operand(in->op == IR_BIN ? in->b : (Operand){OPD_NONE, 0, NULL});
            fputc('}', f);
        }
        fputs("]}", f);
    }
    fputc(']', f);
}

void json_emit(FILE *out, const Compilation *c)
{
    f = out;
    fputc('{', f);
    key("file"), str(c->filename);
    fprintf(f, ",\"opt\":%d,", c->opt_level);

    key("tokens");
    if (c->have_tokens) emit_tokens(&c->tokens); else fputs("null", f);
    fputc(',', f), key("ast");
    if (c->ast) emit_node(c->ast); else fputs("null", f);
    fputc(',', f), key("symbols");
    if (c->have_symbols) emit_symbols(&c->symbols); else fputs("null", f);
    fputc(',', f), key("diagnostics"), emit_diags(&c->diags);
    fputc(',', f), key("ir");
    if (c->ir) emit_ir(c->ir); else fputs("null", f);
    fputc(',', f), key("ir_opt");
    if (c->ir_opt) emit_ir(c->ir_opt); else fputs("null", f);

    fputc(',', f), key("opt_log");
    if (c->ir_opt) {
        fputc('[', f);
        for (int i = 0; i < c->opt_log.count; i++) {
            const OptEntry *e = &c->opt_log.items[i];
            fprintf(f, "%s{", i ? "," : "");
            key("pass"), str(e->pass);
            fputc(',', f), key("function"), str(e->func);
            fputc(',', f), key("before"), str(e->before);
            fputc(',', f), key("after"), str(e->after);
            fputc('}', f);
        }
        fputc(']', f);
    } else {
        fputs("null", f);
    }

    fputc(',', f), key("asm");
    if (c->have_asm) str(c->asm_text.data); else fputs("null", f);
    fputc(',', f), key("failed_stage");
    if (c->failed_stage) str(c->failed_stage); else fputs("null", f);
    fputs("}\n", f);
}
