// c_part.js - Part 1 (mini C compiler) stage definitions for the visualizer.
//
// Each stage object tells app.js how to decide its status, how to draw its
// panel from the /api/c/compile result, and what text to send to the AI tutor.
'use strict';

(function () {
  // Order of minic's own stages, as named in `failed_stage`.
  const ORDER = ['lexer', 'parser', 'sema', 'ir', 'opt', 'asm', 'link', 'run'];
  const failIndex = (r) => {
    if (!r.failed_stage) return (r.run && (r.run.timed_out || r.run.signal)) ? ORDER.indexOf('run') : -1;
    const stage = r.failed_stage === 'assemble' ? 'link' : r.failed_stage;
    return ORDER.indexOf(stage);
  };
  const statusOf = (id) => (r) => {
    const f = failIndex(r);
    const i = ORDER.indexOf(id);
    if (f < 0 || i < f) return 'ok';
    return i === f ? 'fail' : 'skip';
  };

  // ------------------------------------------------------------ shared pieces
  function diagnostics(r, ctrl, stage) {
    const list = (r.diagnostics || []).filter((d) => !stage || d.stage === stage);
    if (!list.length) return document.createDocumentFragment();
    return el('div', null, el('h4', { text: 'Errors (' + list.length + ') - click one to jump to the line' }),
      table(['Position', 'Stage', 'Message'], list.map((d) => el('tr', {
        class: 'diag clickable',
        onclick: () => { ctrl.editor.setHighlights([{ line: d.line, cls: 'hl-err' }]); ctrl.editor.revealLine(d.line); },
      }, el('td', { text: d.line + ':' + d.col }), el('td', { text: d.stage }), el('td', { text: d.message })))));
  }

  function irText(funcs) {
    return (funcs || []).map((fn) => 'function ' + fn.name + '(' + fn.params.join(', ') + ') -> ' + fn.returns + ':\n' +
      fn.instrs.map((i) => (i.op === 'label' ? '  ' : '    ') + i.text).join('\n')).join('\n\n');
  }

  function astText(node, depth) {
    return '  '.repeat(depth) + node.label + '\n' + node.children.map((c) => astText(c, depth + 1)).join('');
  }

  // An IR listing whose lines highlight the source line they came from.
  function irList(funcs, ctrl, onHover) {
    const box = el('div', { class: 'ir-list' });
    (funcs || []).forEach((fn, fi) => {
      box.append(el('div', { class: 'ir-line func', text: 'function ' + fn.name + '(' + fn.params.join(', ') + ') -> ' + fn.returns + ':' }));
      fn.instrs.forEach((ins, ii) => {
        box.append(el('div', {
          class: 'ir-line', dataset: { key: fi + ':' + ii },
          text: (ins.op === 'label' ? '  ' : '    ') + ins.text,
          onmouseenter: () => { ctrl.editor.setHighlights([{ line: ins.line, cls: 'hl-line' }]); if (onHover) onHover(fi + ':' + ii); },
        }));
      });
    });
    return box;
  }

  // ------------------------------------------------------------ 1. tokens
  const tokenClass = (kind) => {
    if (kind.startsWith('KW_')) return 'kw';
    if (kind === 'IDENT') return 'ident';
    if (kind === 'NUMBER') return 'num';
    if (kind === 'EOF') return 'eof';
    if (['LPAREN', 'RPAREN', 'LBRACE', 'RBRACE', 'COMMA', 'SEMI'].includes(kind)) return 'punct';
    return 'op';
  };

  function renderTokens(body, r, ctrl) {
    body.append(diagnostics(r, ctrl, 'lexer'));
    const tokens = r.tokens || [];
    body.append(el('div', { class: 'legend' }, tokens.length + ' tokens. Hover a token to see where it is in the source. ',
      ['kw:keyword', 'ident:identifier', 'num:number', 'op:operator', 'punct:punctuation', 'eof:end of file']
        .map((x) => el('span', { class: 'chip ' + x.split(':')[0], text: x.split(':')[1] }))));
    const chips = el('div', { class: 'chips' });
    let line = 0;
    for (const t of tokens) {
      if (t.line !== line) {
        if (line) chips.append(el('span', { class: 'line-break' }));
        line = t.line;
        chips.append(el('span', { class: 'line-tag', text: line + ':' }));
      }
      chips.append(el('span', {
        class: 'chip ' + tokenClass(t.kind), text: t.display, title: 'line ' + t.line + ', column ' + t.col,
        onmouseenter: () => { ctrl.editor.setHighlights([{ line: t.line, col: t.col, endCol: t.col + Math.max(1, t.len) }]); ctrl.editor.revealLine(t.line); },
      }));
    }
    body.append(chips);
  }

  // ------------------------------------------------------------ 2. AST
  const CHAR_W = 7.3, NODE_H = 26, LEVEL_H = 58, GAP = 14;
  const nodeClass = (kind) => {
    if (['Number', 'Var'].includes(kind)) return 'k-leaf';
    if (['Program', 'Function', 'Param', 'VarDecl'].includes(kind)) return 'k-decl';
    if (['Block', 'Return', 'If', 'While', 'ExprStmt'].includes(kind)) return 'k-stmt';
    return 'k-expr';
  };

  // Draws a tree with collapsible nodes. Each node: {label, children, ...}.
  function drawTree(root, onSelect, collapsed) {
    const wrap = el('div', { class: 'svg-wrap' });
    const redraw = () => {
      // Pass 1: measure. A subtree is as wide as its children side by side.
      const measure = (n) => {
        n._w = Math.max(40, n.label.length * CHAR_W + 18);
        const kids = collapsed.has(n) ? [] : n.children;
        n._kids = kids;
        kids.forEach(measure);
        const kidsWidth = kids.reduce((s, k) => s + k._sub, 0) + GAP * Math.max(0, kids.length - 1);
        n._sub = Math.max(n._w, kidsWidth);
      };
      // Pass 2: place. A parent is centered over its children.
      let maxDepth = 0;
      const place = (n, left, depth) => {
        maxDepth = Math.max(maxDepth, depth);
        n._x = left + n._sub / 2;
        n._y = 12 + depth * LEVEL_H;
        const kidsWidth = n._kids.reduce((s, k) => s + k._sub, 0) + GAP * Math.max(0, n._kids.length - 1);
        let x = left + (n._sub - kidsWidth) / 2;
        for (const k of n._kids) { place(k, x, depth + 1); x += k._sub + GAP; }
      };
      measure(root);
      place(root, 10, 0);
      const svg = svgEl('svg', { width: root._sub + 20, height: (maxDepth + 1) * LEVEL_H + 10 });
      const edges = svgEl('g');
      const nodes = svgEl('g');
      const draw = (n) => {
        for (const k of n._kids) {
          edges.append(svgEl('path', { class: 'tree-edge', d: 'M' + n._x + ',' + (n._y + NODE_H) + ' C' + n._x + ',' + (n._y + NODE_H + 18) + ' ' + k._x + ',' + (k._y - 18) + ' ' + k._x + ',' + k._y }));
          draw(k);
        }
        const isCollapsed = collapsed.has(n);
        const g = svgEl('g', { class: 'tree-node ' + nodeClass(n.kind) + (isCollapsed ? ' collapsed' : '') + (n === drawTree.selected ? ' selected' : '') });
        g.append(svgEl('rect', { x: n._x - n._w / 2, y: n._y, width: n._w, height: NODE_H, rx: 5 }));
        g.append(svgEl('text', { x: n._x, y: n._y + 17, 'text-anchor': 'middle', text: n.label }));
        g.append(svgEl('title', { text: 'lines ' + n.line + ':' + n.col + ' to ' + n.end_line + ':' + n.end_col + (n.children.length ? '\nDouble-click to collapse or expand' : '') }));
        g.addEventListener('click', () => { drawTree.selected = n; onSelect(n); redraw(); });
        g.addEventListener('dblclick', () => {
          if (!n.children.length) return;
          if (collapsed.has(n)) collapsed.delete(n); else collapsed.add(n);
          redraw();
        });
        nodes.append(g);
        if (isCollapsed) nodes.append(svgEl('text', { x: n._x, y: n._y + NODE_H + 14, 'text-anchor': 'middle', fill: '#64748b', text: '+' + countNodes(n) + ' hidden' }));
      };
      draw(root);
      svg.append(edges, nodes);
      wrap.replaceChildren(svg);
    };
    redraw();
    return wrap;
  }
  const countNodes = (n) => n.children.reduce((s, c) => s + 1 + countNodes(c), 0);

  function renderAst(body, r, ctrl) {
    body.append(diagnostics(r, ctrl, 'parser'));
    if (!r.ast) return;
    const collapsed = new Set();
    const select = (n) => {
      ctrl.editor.setHighlights([{ line: n.line, col: n.col, endLine: n.end_line, endCol: n.end_col, cls: 'hl-node' }]);
      ctrl.editor.revealLine(n.line);
    };
    const holder = el('div');
    const draw = () => holder.replaceChildren(drawTree(r.ast, select, collapsed));
    body.append(el('div', { class: 'legend' }, 'Click a node to highlight its source range. Double-click to collapse or expand a subtree. ',
      el('button', { class: 'btn', text: 'Expand all', onclick: () => { collapsed.clear(); draw(); } }),
      el('button', { class: 'btn', text: 'Collapse function bodies', onclick: () => {
        collapsed.clear();
        for (const fn of r.ast.children) for (const c of fn.children) if (c.kind === 'Block') collapsed.add(c);
        draw();
      } })));
    body.append(holder);
    draw();
  }

  // ------------------------------------------------------------ 3. semantic
  function renderSema(body, r, ctrl) {
    body.append(diagnostics(r, ctrl, 'sema'));
    if (!(r.diagnostics || []).some((d) => d.stage === 'sema')) body.append(note('No semantic errors.', 'good'));
    const scopes = r.symbols || [];
    const depth = (s) => (s.parent < 0 ? 0 : 1 + depth(scopes.find((x) => x.id === s.parent)));
    body.append(el('h4', { text: 'Symbol table, one box per scope (inner scopes are indented)' }));
    for (const s of scopes) {
      const box = el('div', { style: { marginLeft: depth(s) * 22 + 'px', marginBottom: '8px' } },
        el('div', null, el('strong', { text: 'Scope ' + s.id + ': ' + s.label }), s.parent >= 0 ? ' (inside scope ' + s.parent + ')' : ''));
      if (!s.symbols.length) box.append(note('no declarations'));
      else {
        box.append(table(['Name', 'Kind', 'Type', 'IR name', 'Declared at'], s.symbols.map((y) => el('tr', {
          class: 'clickable hover-row',
          onclick: () => { if (y.line) { ctrl.editor.setHighlights([{ line: y.line, col: y.col, endCol: y.col + 1e6, cls: 'hl-node' }]); ctrl.editor.revealLine(y.line); } },
        }, el('td', { text: y.name }), el('td', { text: y.kind }),
        el('td', { text: y.kind.includes('function') ? y.type + ' (' + y.nparams + ' parameter' + (y.nparams === 1 ? '' : 's') + ')' : y.type }),
        el('td', { text: y.ir_name || '' }), el('td', { text: y.line ? y.line + ':' + y.col : 'built in' })))));
      }
      body.append(box);
    }
  }

  // ------------------------------------------------------------ 4. IR
  function renderIr(body, r, ctrl) {
    body.append(note('Hover a row to highlight the source line it was generated from.'));
    for (const fn of r.ir || []) {
      body.append(el('h4', { text: 'function ' + fn.name + '(' + fn.params.join(', ') + ') -> ' + fn.returns }));
      body.append(table(['#', 'Kind', 'dst', 'arg1', 'op', 'arg2', 'Instruction', 'Line'], fn.instrs.map((ins, i) => el('tr', {
        class: 'hover-row', onmouseenter: () => { ctrl.editor.setHighlights([{ line: ins.line, cls: 'hl-line' }]); },
      }, [i + 1, ins.op, ins.dst, ins.arg1, ins.operator, ins.arg2, ins.text, ins.line].map((c) => el('td', { text: c }))))));
    }
  }

  // ------------------------------------------------------------ 5. optimizer
  function renderOpt(body, r, ctrl) {
    if (!r.ir_opt) {
      body.append(note('The optimizer is off at -O0, so the code generator uses the IR exactly as generated. Select -O1 next to the Compile button and compile again to see the optimizer work.', 'warn'));
      return;
    }
    const count = (funcs) => funcs.reduce((s, f) => s + f.instrs.length, 0);
    body.append(note('IR instructions: ' + count(r.ir) + ' before, ' + count(r.ir_opt) + ' after. Changes applied: ' + r.opt_log.length + '.', 'good'));
    body.append(el('div', { class: 'cols' },
      el('div', null, el('h4', { text: 'Before (-O0 IR)' }), irList(r.ir, ctrl)),
      el('div', null, el('h4', { text: 'After (-O1 IR)' }), irList(r.ir_opt, ctrl))));
    const passes = {};
    for (const e of r.opt_log) passes[e.pass] = (passes[e.pass] || 0) + 1;
    body.append(el('h4', { text: 'Optimization log (in the order the changes were made)' }));
    body.append(el('div', { class: 'legend' }, Object.entries(passes).map(([p, n]) => el('span', { class: 'badge info', text: p + ': ' + n }))));
    if (!r.opt_log.length) body.append(note('Nothing to optimize in this program.'));
    const shown = r.opt_log.slice(0, 400);
    appendKids(body, shown.map((e, i) => el('div', { class: 'log-entry' }, el('span', { text: (i + 1) + '. ' }), el('span', { class: 'pass', text: e.pass }),
      ' in ' + e.function + ': ', el('span', { class: 'before', text: e.before }), ' \u2192 ', el('span', { class: 'after', text: e.after }))));
    if (r.opt_log.length > shown.length) body.append(note('Showing the first ' + shown.length + ' of ' + r.opt_log.length + ' entries.', 'warn'));
  }

  // ------------------------------------------------------------ 6. assembly
  const REGS = /\b(r[abcd]x|e[abcd]x|[abcd]l|r[sb]p|e[sb]p|r[sd]i|e[sd]i|[sd]il|r(?:8|9|1[0-5])[dwb]?|rip|xmm\d+)\b/g;

  // Returns a <div> for one assembly line with simple syntax highlighting.
  function asmLine(text) {
    const div = el('div', { class: 'asm-line' });
    const hash = text.indexOf('#');
    let code = hash >= 0 ? text.slice(0, hash) : text;
    const comment = hash >= 0 ? text.slice(hash) : '';
    const m = /^(\s*)([.\w$@]+:)?(\s*)([.\w]+)?(.*)$/.exec(code) || [];
    if (m[1]) div.append(m[1]);
    if (m[2]) div.append(el('span', { class: 't-label', text: m[2] }));
    if (m[3]) div.append(m[3]);
    if (m[4]) div.append(el('span', { class: m[4].startsWith('.') ? 't-directive' : 't-mnemonic', text: m[4] }));
    const rest = m[5] || '';
    // Split operands into registers, numbers, size keywords and plain text.
    const re = new RegExp(REGS.source + '|\\b(DWORD|QWORD|BYTE|WORD|PTR)\\b|(?<![\\w.])-?\\b\\d+\\b|(?<![\\w.])0x[0-9a-f]+\\b', 'g');
    let last = 0, t;
    while ((t = re.exec(rest))) {
      if (t.index > last) div.append(rest.slice(last, t.index));
      const cls = t[1] ? 't-reg' : t[2] ? 't-kw' : 't-num';
      div.append(el('span', { class: cls, text: t[0] }));
      last = t.index + t[0].length;
    }
    if (last < rest.length) div.append(rest.slice(last));
    if (comment) div.append(el('span', { class: 't-comment', text: comment }));
    return div;
  }

  function asmBlock(text) {
    return el('div', { class: 'asm-list' }, String(text || '').replace(/\n$/, '').split('\n').map(asmLine));
  }

  // minic assembly, grouped by its `# <IR instruction>` comments.
  function renderAsm(body, r, ctrl) {
    const funcs = r.opt >= 1 && r.ir_opt ? r.ir_opt : r.ir;
    body.append(note('Left: the IR that was translated (' + (r.opt >= 1 ? 'optimized, -O1' : 'unoptimized, -O0') + '). Right: the assembly written by minic. Hover an IR line to highlight the machine instructions generated for it, or hover an assembly group to find its IR line.'));
    const groups = {};
    const irLines = {};
    const activate = (key, scroll) => {
      for (const [k, g] of Object.entries(groups)) g.classList.toggle('active', k === key);
      for (const [k, l] of Object.entries(irLines)) l.classList.toggle('active', k === key);
      if (scroll && groups[key]) groups[key].scrollIntoView({ block: 'nearest' });
    };
    const irBox = irList(funcs, ctrl, (key) => activate(key, true));
    for (const line of irBox.querySelectorAll('.ir-line[data-key]')) irLines[line.dataset.key] = line;

    const asmBox = el('div', { class: 'asm-list' });
    let fi = -1, ii = 0, group = null;
    for (const text of r.asm.replace(/\n$/, '').split('\n')) {
      const label = /^([A-Za-z_]\w*):/.exec(text);
      if (label) { fi = funcs.findIndex((f) => f.name === label[1]); ii = 0; group = null; }
      const comment = /^\s+# (.*)$/.exec(text);
      const fn = funcs[fi];
      if (comment && fn && ii < fn.instrs.length && fn.instrs[ii].text === comment[1]) {
        const key = fi + ':' + ii;
        const line = fn.instrs[ii].line;
        group = el('div', { class: 'asm-group', onmouseenter: () => { activate(key, false); ctrl.editor.setHighlights([{ line, cls: 'hl-line' }]); if (irLines[key]) irLines[key].scrollIntoView({ block: 'nearest' }); } });
        groups[key] = group;
        asmBox.append(group);
        ii++;
      } else if (/^\s+\.size|^\s*$/.test(text)) {
        group = null;
      }
      (group || asmBox).append(asmLine(text));
    }
    body.append(el('div', { class: 'cols' }, el('div', null, el('h4', { text: 'IR' }), irBox),
      el('div', null, el('h4', { text: 'x86-64 assembly (Intel syntax) - output of minic' }), asmBox)));
  }

  // ------------------------------------------------------------ 7. link / ELF / GCC inspector
  const sectionClass = (name) => {
    if (name === '.text' || name.startsWith('.text.') || name === '.init' || name === '.fini') return 'text';
    if (name.startsWith('.rodata')) return 'rodata';
    if (name === '.data' || name.startsWith('.data.')) return 'data';
    if (name === '.bss') return 'bss';
    if (['.symtab', '.strtab', '.shstrtab', '.dynsym', '.dynstr'].includes(name)) return 'sym';
    if (name.startsWith('.rela') || name.startsWith('.rel.')) return 'rel';
    if (name.startsWith('.plt') || name.startsWith('.got') || name === '.dynamic' || name === '.interp') return 'dyn';
    return 'other';
  };
  const hex = (n) => '0x' + n.toString(16);

  // File layout of one ELF file: header, program headers, sections, section headers.
  function elfFileDiagram(title, elf) {
    const box = el('div', { class: 'elf-file' }, el('h4', { text: title + ' (' + elf.file_size + ' bytes)' }));
    const row = (cls, name, detail) => box.append(el('div', { class: 'elf-box ' + cls }, el('span', { text: name }), el('span', { class: 'sz', text: detail })));
    row('hdr', 'ELF header', 'offset 0x0, 64 bytes');
    const nph = Number(elf.num_program_headers) || 0;
    if (nph) row('ph', 'Program header table', nph + ' entries x 56 bytes');
    const sections = elf.sections.slice().sort((a, b) => a.offset - b.offset || a.index - b.index);
    for (const s of sections) {
      const inFile = s.type !== 'NOBITS';
      row(sectionClass(s.name), s.name, hex(s.offset) + ', ' + s.size + ' bytes' + (inFile ? '' : ' in memory only') + (s.flags ? ' [' + s.flags + ']' : ''));
    }
    row('sh', 'Section header table', (elf.num_section_headers || '?') + ' entries x 64 bytes');
    return box;
  }

  function elfView(g) {
    const o = g.object.elf, x = g.executable.elf;
    const has = (elf, re) => elf.sections.some((s) => re.test(s.name));
    const nmExe = g.executable.tools.nm.stdout;
    const rows = [
      ['ELF type', o.type, x.type],
      ['Entry point address', o.entry, x.entry],
      ['Program headers (segments)', o.num_program_headers, x.num_program_headers],
      ['Section headers', o.num_section_headers, x.num_section_headers],
      ['Relocation entries', o.num_relocations + ' (to be fixed by the linker)', x.num_relocations + ' (left for the dynamic loader)'],
      ['Undefined symbols', o.undefined_symbols.join(', ') || 'none', x.undefined_symbols.join(', ') || 'none'],
      ['_start symbol', 'no', /\b[Tt] _start\b/.test(nmExe) ? 'yes (from crt1.o, calls main)' : 'no'],
      ['PLT / GOT sections', has(o, /^\.(plt|got)/) ? 'yes' : 'no', has(x, /^\.(plt|got)/) ? 'yes' : 'no'],
      ['Dynamic linking info (.interp, .dynamic)', has(o, /^\.(interp|dynamic)$/) ? 'yes' : 'no', has(x, /^\.(interp|dynamic)$/) ? 'yes' : 'no'],
      ['File size', o.file_size + ' bytes', x.file_size + ' bytes'],
    ];
    const cmp = table(['', 'prog.o (object file)', 'prog (executable)'], rows.map((row) => el('tr', { class: String(row[1]) !== String(row[2]) ? 'diff-yes' : '' }, row.map((c) => el('td', { text: c })))));
    const segs = table(['Segment', 'Offset', 'Virtual address', 'File size', 'Memory size', 'Flags', 'Sections inside'], x.segments.map((s, i) =>
      [s.type, hex(s.offset), hex(s.vaddr), s.filesz, s.memsz, s.flags, (x.segment_sections[i] || []).join(' ')]));
    return el('div', null,
      el('p', { class: 'note' }, 'An ELF file has two views of the same bytes. Sections (.text, .data, ...) are the linker\'s view: named pieces it merges and relocates. Segments (program headers) are the loader\'s view: which byte ranges to map into memory and with which permissions. An object file only needs sections; an executable needs segments.'),
      el('h4', { text: 'Object file vs executable (differing rows are highlighted)' }), cmp,
      el('ul', null,
        el('li', { text: 'REL = relocatable: addresses are not final, entry point is 0, there are no program headers, and calls to other symbols are recorded as relocation entries for the linker.' }),
        el('li', { text: 'DYN (position-independent executable) or EXEC: the linker merged sections from prog.o and the C runtime (crt1.o, crti.o, ...), assigned addresses, resolved relocations, and set the entry point to _start, which later calls main.' }),
        el('li', { text: 'printf lives in libc.so, so it stays undefined in the executable. The call goes through the PLT, and the GOT entry is filled in by the dynamic loader named in .interp.' })),
      el('h4', { text: 'File layout (in file offset order)' }),
      el('div', { class: 'elf-diagram' }, elfFileDiagram('prog.o', o), elfFileDiagram('prog', x)),
      el('div', { class: 'legend' }, ['hdr:headers', 'ph:program headers', 'text:code', 'rodata:read-only data', 'data:data', 'bss:bss', 'sym:symbols/strings', 'rel:relocations', 'dyn:dynamic linking', 'other:other']
        .map((s) => el('span', { class: 'elf-box ' + s.split(':')[0], style: { borderTopWidth: '1px' }, text: s.split(':')[1] }))),
      el('h4', { text: 'Segments of the executable (what the loader maps)' }), segs);
  }

  function toolsView(g, ctrl) {
    const state = { file: 'object', tool: ctrl.params.tool || 'readelf -h' };
    const out = el('div');
    const names = Object.keys(g.object.tools);
    const fileSel = el('select', null, el('option', { value: 'object', text: 'prog.o (object file)' }), el('option', { value: 'executable', text: 'prog (executable)' }));
    const toolSel = el('select', null, names.map((n) => el('option', { value: n, text: n })));
    toolSel.value = state.tool;
    if (ctrl.params.file === 'executable') { fileSel.value = 'executable'; state.file = 'executable'; }
    const show = () => out.replaceChildren(cmdResult(g[state.file].tools[state.tool]));
    fileSel.addEventListener('change', () => { state.file = fileSel.value; show(); });
    toolSel.addEventListener('change', () => { state.tool = toolSel.value; show(); });
    show();
    return el('div', null, el('div', { class: 'toolbar' }, el('label', null, 'File ', fileSel), el('label', null, 'Tool ', toolSel)), out);
  }

  function pipelineView(g) {
    const tail = (text, n) => { const lines = text.replace(/\n$/, '').split('\n'); return (lines.length > n ? '... (' + (lines.length - n) + ' earlier lines, mostly from <stdio.h>, not shown)\n' : '') + lines.slice(-n).join('\n'); };
    return el('div', null,
      el('p', { class: 'note warn' }, 'print() is a minic built-in, not real C. So that GCC accepts the same source, these lines are added in front of it automatically (the prototypes are needed because minic lets you call a function before its definition):'),
      codeBlock(g.shim),
      el('h4', { text: '1. Preprocess: prog.c -> prog.i' }), cmdResult(g.preprocess), g.preprocessed ? codeBlock(tail(g.preprocessed, 40)) : null,
      el('h4', { text: '2. Compile: prog.c -> prog.s (assembly)' }), cmdResult(g.compile), g.asm ? asmBlock(g.asm) : null,
      el('h4', { text: '3. Assemble: -> prog.o (object file)' }), cmdResult(g.assemble),
      el('h4', { text: '4. Link: prog.o + C runtime + libc -> prog (executable)' }), cmdResult(g.link),
      el('h4', { text: '5. Run' }), cmdResult(g.run, { neutralExit: true }));
  }

  function compareView(g, r) {
    return el('div', null,
      note('The same program from three compilers. minic and gcc -O0 keep every variable on the stack; gcc -O2 keeps values in registers and may compute the whole result at compile time. The gcc listings use extra flags (' + g.compare.flags + ') to hide unwind tables and security instrumentation.'),
      el('div', { class: 'cols' },
        el('div', null, el('h4', { text: 'minic -O' + r.opt }), asmBlock(r.asm)),
        el('div', null, el('h4', { text: 'gcc -O0' }), asmBlock(g.compare.O0.asm)),
        el('div', null, el('h4', { text: 'gcc -O2' }), asmBlock(g.compare.O2.asm))));
  }

  function renderLink(body, r, ctrl) {
    appendKids(body, [el('h4', { text: 'Finishing minic\'s output with the GNU toolchain' }),
      note('minic stops at assembly text (prog.s). The assembler `as` produces the object file, and `gcc` (which runs the linker `ld` with the C runtime and libc) produces the ELF executable.'),
      el('div', { class: 'cmd-result' }, el('div', { class: 'cmd-line' }, el('span', { class: 'prompt', text: '$ ' }), r.minic_cmd, el('span', { class: 'badge ok', text: 'minic: assembly text' }))),
      cmdResult(r.assemble), r.link ? cmdResult(r.link) : null]);

    body.append(el('h3', { text: 'Real GCC pipeline inspector' }));
    const holder = el('div');
    const optSel = el('select', null, [0, 1, 2, 3].map((n) => el('option', { value: n, text: '-O' + n })));
    optSel.value = ctrl.params.gccopt || '0';
    body.append(el('div', { class: 'toolbar' }, el('label', null, 'GCC optimization level ', optSel)), holder);
    const source = ctrl.editor.value;
    ctrl.gccCache = ctrl.gccCache && ctrl.gccCache.source === source ? ctrl.gccCache : { source, byOpt: {} };
    const load = async () => {
      const opt = Number(optSel.value);
      holder.replaceChildren(note('Running gcc, readelf, objdump, nm ...'));
      try {
        const g = ctrl.gccCache.byOpt[opt] || (ctrl.gccCache.byOpt[opt] = await api('/api/c/gcc', { source, opt }));
        ctrl.gcc = g;
        if (!g.ok) {
          holder.replaceChildren(note('GCC stopped at step: ' + g.failed_step + '. (GCC compiles real C, so it can reject or accept programs differently from minic.)', 'error'),
            cmdResult(g[g.failed_step]));
        } else {
          holder.replaceChildren(subTabs([
            { id: 'elf', label: 'ELF view', render: () => elfView(g) },
            { id: 'pipeline', label: 'Pipeline steps', render: () => pipelineView(g) },
            { id: 'tools', label: 'Tools (readelf, objdump, nm, ...)', render: () => toolsView(g, ctrl) },
            { id: 'compare', label: 'Assembly side by side', render: () => compareView(g, r) },
          ], ctrl.params.sub || 'elf', (id) => { ctrl.params.sub = id; }));
        }
      } catch (e) {
        holder.replaceChildren(note(e.message, 'error'));
      }
      document.body.dataset.gcc = String((Number(document.body.dataset.gcc) || 0) + 1);
    };
    optSel.addEventListener('change', load);
    load();
  }

  // ------------------------------------------------------------ 8. run
  function renderRun(body, r) {
    body.append(cmdResult(r.run, { neutralExit: true }));
    if (!r.run) return;
    if (r.run.timed_out) body.append(note('The program did not finish within the time limit and was killed (an infinite loop?).', 'error'));
    else if (r.run.signal === 'SIGXFSZ') body.append(note('The program wrote more output than the limit allows and was stopped (a loop that prints forever?).', 'error'));
    else if (r.run.signal) body.append(note('The program was killed by ' + r.run.signal + (r.run.signal === 'SIGFPE' ? ' (division by zero).' : r.run.signal === 'SIGSEGV' ? ' (most likely unbounded recursion overflowing the stack).' : '.'), 'error'));
    else body.append(note('Exit code ' + r.run.exit_code + ' is the value returned by main (only the low 8 bits reach the shell, so 300 would show as 44).', 'good'));
    if (!r.run.stdout) body.append(note('The program printed nothing. Use print(x) to write an integer to stdout.'));
  }

  // ------------------------------------------------------------ stage table
  const optSelect = el('select', { title: 'minic optimization level' }, el('option', { value: '0', text: '-O0 (optimizer off)' }), el('option', { value: '1', text: '-O1 (optimizer on)' }));

  App.parts.c = {
    id: 'c',
    defaultExample: '01_minimal.c',
    controls: (ctrl) => {
      if (App.params.opt === '1' && App.params.part !== 'ai') optSelect.value = '1';
      optSelect.addEventListener('change', () => ctrl.compile());
      return el('label', null, 'minic ', optSelect);
    },
    compile: (source) => api('/api/c/compile', { source, opt: Number(optSelect.value) }),
    stages: [
      {
        id: 'tokens', label: 'Lexer', status: statusOf('lexer'), render: renderTokens,
        help: ['The lexer (lexer.c) reads the source one character at a time and groups characters into tokens: keywords, identifiers, numbers, operators and punctuation. Whitespace and comments disappear here.',
          'Each token remembers its line and column, so later stages can report errors at the right place. The lexer knows nothing about grammar: "int int +" is a perfectly good token stream.'],
        text: (r) => (r.tokens || []).map((t) => t.line + ':' + t.col + ' ' + t.display).join('\n') + '\n' + JSON.stringify(r.diagnostics),
      },
      {
        id: 'ast', label: 'Parser', status: statusOf('parser'), render: renderAst,
        help: ['The parser (parser.c) checks that the tokens follow the grammar and builds the abstract syntax tree (AST). It is a hand-written recursive-descent parser: one function per grammar rule.',
          'Precedence is encoded in the tree shape: in "a + b * 2" the * node sits below the + node, so it is evaluated first. Parentheses and semicolons leave no nodes behind; they only shape the tree.'],
        text: (r) => (r.ast ? astText(r.ast, 0) : '') + '\n' + JSON.stringify(r.diagnostics),
      },
      {
        id: 'sema', label: 'Semantic', status: statusOf('sema'), render: renderSema,
        help: ['Semantic analysis (sema.c) checks what the grammar cannot: every name must be declared, not declared twice in one scope, functions must be called with the right number of arguments, and a void result cannot be used as a value.',
          'It builds a symbol table with one scope per function and per block. Unlike the lexer and parser, it keeps going after an error so you see all problems at once. Each variable also gets a unique IR name, so shadowed variables stay distinct later.'],
        text: (r) => JSON.stringify({ scopes: r.symbols, diagnostics: r.diagnostics }, null, 1),
      },
      {
        id: 'ir', label: 'IR', status: statusOf('ir'), render: renderIr,
        help: ['The IR generator (ir.c) flattens the tree into three-address code: a list of tiny instructions with at most one operator each. Intermediate results go into temporaries t1, t2, ...',
          'if and while turn into labels, "jump L" and "branch_if_zero x, L". This flat form is what optimizers and code generators work on in real compilers too (GIMPLE in GCC, LLVM IR in Clang).'],
        text: (r) => irText(r.ir),
      },
      {
        id: 'opt', label: 'Optimizer', status: statusOf('opt'), render: renderOpt,
        help: ['The optimizer (opt.c) rewrites the IR into a cheaper but equivalent IR. It runs a sequence of small passes again and again until nothing changes, because each pass creates opportunities for the others.',
          'Passes: constant and copy propagation, constant folding, algebraic simplification, common subexpression elimination, branch folding, unreachable code elimination, and dead store elimination. Every change is logged below. Division by zero is never folded: it must still fail at run time.'],
        text: (r) => (r.ir_opt ? 'BEFORE:\n' + irText(r.ir) + '\n\nAFTER:\n' + irText(r.ir_opt) + '\n\nLOG:\n' + r.opt_log.map((e) => e.pass + ' [' + e.function + ']: ' + e.before + ' -> ' + e.after).join('\n') : 'Optimizer off (-O0).\n' + irText(r.ir)),
      },
      {
        id: 'asm', label: 'Assembly', status: statusOf('asm'), render: renderAsm,
        help: ['The code generator (codegen.c) translates each IR instruction into x86-64 instructions in Intel syntax, following the System V calling convention: arguments in edi, esi, edx, ecx, r8d, r9d and the result in eax.',
          'The strategy is the simplest that works: every variable and temporary has its own 4-byte slot on the stack ([rbp-4], [rbp-8], ...), and eax is used as scratch. This is the last thing minic itself produces.'],
        text: (r) => r.asm || '',
      },
      {
        id: 'link', label: 'Link / ELF', status: statusOf('link'), render: renderLink,
        help: ['Assembly text is not a program yet. The assembler (as) encodes it into machine code in an object file (prog.o). The linker (ld, run by gcc) combines it with the C runtime startup code and connects the call to printf with libc, producing an ELF executable.',
          'These steps are done by the GNU toolchain, not by minic. The inspector below runs the real GCC pipeline on the same source so you can look inside the object file and the executable with readelf, objdump and nm.'],
        text: (r, ctrl) => {
          const g = ctrl.gcc;
          let s = [r.minic_cmd, r.assemble && r.assemble.cmd, r.link && r.link.cmd].filter(Boolean).join('\n');
          if (g && g.ok) s += '\n\nreadelf -h prog.o:\n' + g.object.tools['readelf -h'].stdout + '\nreadelf -h prog:\n' + g.executable.tools['readelf -h'].stdout + '\nreadelf -r prog.o:\n' + g.object.tools['readelf -r'].stdout + '\nreadelf -l prog:\n' + g.executable.tools['readelf -l'].stdout;
          return s;
        },
      },
      {
        id: 'run', label: 'Run', status: statusOf('run'), render: renderRun,
        help: ['The linked executable is started as a normal process (with a time limit). The kernel loads the ELF segments, the dynamic loader maps libc, _start calls main, and the value main returns becomes the process exit code.'],
        text: (r) => (r.run ? 'stdout:\n' + r.run.stdout + '\n' + exitText(r.run) : 'not run'),
      },
    ],
  };
})();
