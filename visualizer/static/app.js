// app.js - shared UI framework for both parts of the visualizer.
//
// Provides: DOM helpers, the API client, the source editor (textarea with
// line numbers and range highlighting), the pipeline bar, the per-stage panel
// with its static explanation, the "Explain with AI" box, and a small safe
// Markdown renderer. c_part.js describes the C compiler stages; lab.js renders adapter sessions.
'use strict';

const App = { health: null, parts: {}, controllers: {}, params: {} };

// ---------------------------------------------------------------- DOM helpers
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  setAttrs(e, attrs);
  appendKids(e, kids);
  return e;
}

function svgEl(tag, attrs, ...kids) {
  const e = document.createElementNS('http://www.w3.org/2000/svg', tag);
  setAttrs(e, attrs);
  appendKids(e, kids);
  return e;
}

function setAttrs(e, attrs) {
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') e.setAttribute('class', v);
    else if (k === 'text') e.textContent = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(e.style, v);
    else if (k === 'dataset') Object.assign(e.dataset, v);
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v === true ? '' : v);
  }
}

function appendKids(e, kids) {
  for (const kid of kids.flat(Infinity)) {
    if (kid == null || kid === false) continue;
    e.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  }
}

function escapeHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function showError(message) {
  const banner = document.getElementById('error-banner');
  banner.textContent = message;
  banner.hidden = !message;
}

window.addEventListener('error', (e) => showError('UI error: ' + e.message + ' (' + (e.filename || '').split('/').pop() + ':' + e.lineno + ')'));
window.addEventListener('unhandledrejection', (e) => showError('UI error: ' + (e.reason && e.reason.message || e.reason)));

// ---------------------------------------------------------------- API client
async function api(path, body) {
  const options = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  };
  let response;
  try {
    response = await fetch(path, options);
  } catch (e) {
    throw new Error('Cannot reach the server. Is it still running?');
  }
  let data = null;
  try { data = await response.json(); } catch (e) { /* not JSON */ }
  if (!response.ok) throw new Error((data && data.error) || ('HTTP ' + response.status));
  return data;
}

// ---------------------------------------------------------------- small widgets
function codeBlock(text, cls) {
  return el('pre', { class: 'code ' + (cls || ''), text: text == null ? '' : text });
}

function note(text, cls) {
  return el('p', { class: 'note ' + (cls || ''), text });
}

function table(headers, rows, cls) {
  return el('table', { class: 'grid ' + (cls || '') },
    el('thead', null, el('tr', null, headers.map((h) => el('th', { text: h })))),
    el('tbody', null, rows.map((r) => (r && r.nodeType) ? r : el('tr', null, r.map((c) => (c && c.nodeType) ? el('td', null, c) : el('td', { text: c == null ? '' : c }))))));
}

// Describes how a subprocess ended: exit code, signal or timeout.
function exitText(r) {
  if (!r) return 'not run';
  if (r.timed_out) return 'timed out after ' + r.timeout_seconds + ' s (killed)';
  if (r.signal) return 'killed by ' + r.signal;
  return 'exit code ' + r.exit_code;
}

// Renders one tool invocation: the command line, how it ended and its output.
function cmdResult(r, opts) {
  opts = opts || {};
  if (!r) return note('Not run.');
  const box = el('div', { class: 'cmd-result' });
  box.append(el('div', { class: 'cmd-line' }, el('span', { class: 'prompt', text: '$ ' }), r.cmd,
    el('span', { class: 'badge ' + (r.ok || opts.neutralExit ? 'ok' : 'fail'), text: exitText(r) })));
  if (r.stdout) box.append(codeBlock(r.stdout, 'out'));
  if (r.stderr) box.append(codeBlock(r.stderr, 'err'));
  if (r.truncated) box.append(note('Output was truncated.', 'warn'));
  return box;
}

// A row of buttons that switch between sub-views. tabs: [{id, label, render()}]
function subTabs(tabs, initial, onChange) {
  const bar = el('div', { class: 'subtabs' });
  const body = el('div', { class: 'subtab-body' });
  const select = (id) => {
    const tab = tabs.find((t) => t.id === id) || tabs[0];
    for (const b of bar.children) b.classList.toggle('active', b.dataset.id === tab.id);
    body.replaceChildren();
    appendKids(body, [tab.render()]);
    if (onChange) onChange(tab.id);
  };
  for (const t of tabs) bar.append(el('button', { class: 'subtab', dataset: { id: t.id }, text: t.label, onclick: () => select(t.id) }));
  const root = el('div', null, bar, body);
  select(initial || tabs[0].id);
  return root;
}

// ---------------------------------------------------------------- Markdown
// Minimal and safe: every piece of text is HTML-escaped before any markup is
// added, and only a fixed set of tags is ever produced.
function mdInline(text) {
  let s = escapeHtml(text);
  s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  s = s.replace(/(^|[^*\w])\*([^*\s][^*]*)\*(?!\w)/g, '$1<em>$2</em>');
  return s;
}

function mdToHtml(md) {
  const lines = String(md).replace(/\r/g, '').split('\n');
  const out = [];
  let list = null;      // 'ul' | 'ol' | null
  let para = [];
  const flushPara = () => { if (para.length) { out.push('<p>' + para.map(mdInline).join(' ') + '</p>'); para = []; } };
  const closeList = () => { if (list) { out.push('</' + list + '>'); list = null; } };
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (/^\s*```/.test(line)) {
      flushPara(); closeList();
      const code = [];
      for (i++; i < lines.length && !/^\s*```/.test(lines[i]); i++) code.push(lines[i]);
      out.push('<pre class="code">' + escapeHtml(code.join('\n')) + '</pre>');
      continue;
    }
    let m;
    if ((m = /^(#{1,6})\s+(.*)$/.exec(line))) {
      flushPara(); closeList();
      const level = Math.min(6, m[1].length + 2);
      out.push('<h' + level + '>' + mdInline(m[2]) + '</h' + level + '>');
    } else if ((m = /^\s*[-*]\s+(.*)$/.exec(line))) {
      flushPara();
      if (list !== 'ul') { closeList(); out.push('<ul>'); list = 'ul'; }
      out.push('<li>' + mdInline(m[1]) + '</li>');
    } else if ((m = /^\s*\d+[.)]\s+(.*)$/.exec(line))) {
      flushPara();
      if (list !== 'ol') { closeList(); out.push('<ol>'); list = 'ol'; }
      out.push('<li>' + mdInline(m[1]) + '</li>');
    } else if (!line.trim()) {
      flushPara(); closeList();
    } else {
      closeList();
      para.push(line.trim());
    }
  }
  flushPara(); closeList();
  return out.join('\n');
}

// ---------------------------------------------------------------- Editor
// A textarea with a line-number gutter. Highlights are drawn in a <pre>
// placed exactly behind the (transparent) textarea.
class Editor {
  constructor(parent) {
    this.gutter = el('div', { class: 'gutter' });
    this.backdrop = el('pre', { class: 'backdrop' });
    this.textarea = el('textarea', { class: 'source', spellcheck: 'false', wrap: 'off', autocomplete: 'off', autocapitalize: 'off' });
    this.root = el('div', { class: 'editor' }, this.gutter, el('div', { class: 'code-wrap' }, this.backdrop, this.textarea));
    parent.append(this.root);
    this.highlights = [];
    this.onChange = null;
    this.textarea.addEventListener('input', () => { this.highlights = []; this.refresh(); if (this.onChange) this.onChange(); });
    this.textarea.addEventListener('scroll', () => this.syncScroll());
    this.textarea.addEventListener('keydown', (e) => {
      if (e.key === 'Tab') {
        e.preventDefault();
        this.textarea.setRangeText('    ', this.textarea.selectionStart, this.textarea.selectionEnd, 'end');
        this.textarea.dispatchEvent(new Event('input'));
      }
    });
    this.refresh();
  }

  get value() { return this.textarea.value; }
  set value(v) { this.textarea.value = v; this.highlights = []; this.refresh(); }

  syncScroll() {
    this.backdrop.scrollTop = this.textarea.scrollTop;
    this.backdrop.scrollLeft = this.textarea.scrollLeft;
    this.gutter.scrollTop = this.textarea.scrollTop;
  }

  // ranges: [{line, col, endLine, endCol, cls}] (1-based, end exclusive).
  // A range with only `line` highlights the whole line.
  setHighlights(ranges) { this.highlights = ranges || []; this.refresh(); }
  clearHighlights() { if (this.highlights.length) { this.highlights = []; this.refresh(); } }

  revealLine(line) {
    const lineHeight = parseFloat(getComputedStyle(this.textarea).lineHeight) || 18;
    const top = (line - 1) * lineHeight;
    if (top < this.textarea.scrollTop || top > this.textarea.scrollTop + this.textarea.clientHeight - 2 * lineHeight) {
      this.textarea.scrollTop = Math.max(0, top - this.textarea.clientHeight / 3);
    }
    this.syncScroll();
  }

  refresh() {
    const text = this.textarea.value;
    const lines = text.split('\n');
    const marked = new Set(this.highlights.map((h) => h.line));
    this.gutter.replaceChildren(...lines.map((_, i) => el('div', { class: marked.has(i + 1) ? 'ln marked' : 'ln', text: i + 1 })));
    const starts = [];
    let offset = 0;
    for (const l of lines) { starts.push(offset); offset += l.length + 1; }
    const toOffset = (line, col) => {
      if (line < 1) return 0;
      if (line > lines.length) return text.length;
      return Math.min(starts[line - 1] + Math.max(0, col - 1), starts[line - 1] + lines[line - 1].length);
    };
    const spans = this.highlights.map((h) => {
      const whole = h.col == null;
      const start = whole ? toOffset(h.line, 1) : toOffset(h.line, h.col);
      const end = whole ? toOffset(h.line, 1e9) : toOffset(h.endLine || h.line, h.endCol != null ? h.endCol : h.col + 1);
      return { start, end: Math.max(end, start), cls: h.cls || 'hl' };
    }).filter((s) => s.end > s.start).sort((a, b) => a.start - b.start);
    let html = '';
    let pos = 0;
    for (const s of spans) {
      if (s.start < pos) continue;
      html += escapeHtml(text.slice(pos, s.start)) + '<mark class="' + s.cls + '">' + escapeHtml(text.slice(s.start, s.end)) + '</mark>';
      pos = s.end;
    }
    html += escapeHtml(text.slice(pos)) + '\n';
    this.backdrop.innerHTML = html;
    this.syncScroll();
  }
}

// ---------------------------------------------------------------- Explain with AI
function explainBox(ctrl) {
  const gem = (App.health && App.health.gemini) || { configured: false };
  const out = el('div', { class: 'ai-output' });
  const question = el('input', { type: 'text', class: 'ask-input', placeholder: 'Ask about this stage...', maxlength: '500' });
  const explainBtn = el('button', { class: 'btn ai', text: 'Explain with AI' });
  const askBtn = el('button', { class: 'btn', text: 'Ask' });
  const run = async (q) => {
    const stage = ctrl.currentStage();
    if (!ctrl.result) { out.replaceChildren(note('Compile something first.')); return; }
    explainBtn.disabled = askBtn.disabled = true;
    out.replaceChildren(note('Asking Gemini...'));
    try {
      const data = await api('/api/explain', {
        part: ctrl.def.id, stage: stage.label, source: ctrl.editor.value,
        stage_output: stage.text ? stage.text(ctrl.result, ctrl) : '',
        language: document.getElementById('lang').value, question: q || undefined,
      });
      const body = el('div', { class: 'md' });
      body.innerHTML = mdToHtml(data.text);
      out.replaceChildren(body, el('p', { class: 'ai-meta', text: 'Model: ' + data.model + (data.cached ? ' (cached answer)' : '') }));
    } catch (e) {
      out.replaceChildren(note(e.message, 'error'));
    } finally {
      explainBtn.disabled = askBtn.disabled = false;
    }
  };
  explainBtn.addEventListener('click', () => run(null));
  askBtn.addEventListener('click', () => { if (question.value.trim()) run(question.value.trim()); });
  question.addEventListener('keydown', (e) => { if (e.key === 'Enter' && question.value.trim()) run(question.value.trim()); });
  const box = el('div', { class: 'explain-box' }, el('div', { class: 'explain-row' }, explainBtn, question, askBtn));
  if (!gem.configured) {
    explainBtn.disabled = askBtn.disabled = question.disabled = true;
    box.append(el('p', { class: 'note' }, 'AI explanations are off because no GEMINI_API_KEY is set. Get a free key at ',
      el('a', { href: 'https://aistudio.google.com/apikey', target: '_blank', rel: 'noopener', text: 'Google AI Studio' }),
      ', put it in the .env file (see .env.example) and restart the server.'));
  }
  box.append(out);
  return box;
}

// ---------------------------------------------------------------- Part controller
// def: { id, exampleLabel, stages: [{id, label, help, status(result), render(body, result, ctrl), text(result)}],
//        controls(ctrl) -> [elements], compile(source, ctrl) -> Promise<result>, defaultExample }
class PartController {
  constructor(root, def) {
    this.root = root;
    this.def = def;
    this.result = null;
    this.error = null;
    this.stageId = def.stages[0].id;
    this.params = {};   // deep-link parameters (only set for the part named in the URL)
    this.exampleSelect = el('select', { class: 'examples', onchange: () => this.loadSelectedExample(true) });
    this.compileBtn = el('button', { class: 'btn primary', text: 'Compile', onclick: () => this.compile() });
    this.statusLine = el('div', { class: 'status-line' });
    const toolbar = el('div', { class: 'toolbar' }, el('label', null, 'Example ', this.exampleSelect),
      def.controls ? def.controls(this) : null, this.compileBtn);
    const left = el('div', { class: 'left' }, toolbar);
    this.editor = new Editor(left);
    left.append(this.statusLine);
    this.bar = el('div', { class: 'pipeline-bar' });
    this.panel = el('div', { class: 'stage-panel' });
    root.append(el('div', { class: 'part-grid' }, left, el('div', { class: 'right' }, this.bar, this.panel)));
    this.editor.textarea.addEventListener('keydown', (e) => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) this.compile(); });
    this.renderBar();
    this.renderStage();
  }

  currentStage() { return this.def.stages.find((s) => s.id === this.stageId) || this.def.stages[0]; }

  async loadExamples(preferred) {
    try {
      const data = await api('/api/examples?part=' + this.def.id);
      this.examples = data.examples;
    } catch (e) {
      this.examples = [];
      showError(e.message);
    }
    this.exampleSelect.replaceChildren(...this.examples.map((x) => el('option', { value: x.name, text: x.name + ' - ' + x.title })));
    const want = preferred || this.def.defaultExample;
    if (this.examples.some((x) => x.name === want)) this.exampleSelect.value = want;
    this.loadSelectedExample(false);
  }

  loadSelectedExample(compileNow) {
    const ex = (this.examples || []).find((x) => x.name === this.exampleSelect.value);
    if (!ex) return;
    this.editor.value = ex.source;
    if (compileNow) this.compile();
  }

  async compile() {
    this.compileBtn.disabled = true;
    this.statusLine.textContent = 'Compiling...';
    this.statusLine.className = 'status-line';
    this.editor.clearHighlights();
    try {
      this.result = await this.def.compile(this.editor.value, this);
      this.error = null;
    } catch (e) {
      this.result = null;
      this.error = e.message;
    }
    this.compileBtn.disabled = false;
    if (this.error) {
      this.statusLine.textContent = this.error;
      this.statusLine.className = 'status-line fail';
    } else {
      const failed = this.def.stages.find((s) => s.status(this.result) === 'fail');
      this.statusLine.textContent = failed ? 'Failed at stage: ' + failed.label : 'All stages succeeded.';
      this.statusLine.className = 'status-line ' + (failed ? 'fail' : 'ok');
      if (failed) this.stageId = failed.id;
      else if (this.currentStage().status(this.result) === 'skip') this.stageId = this.def.stages[0].id;
    }
    this.renderBar();
    this.renderStage();
    document.body.dataset.compiled = String((Number(document.body.dataset.compiled) || 0) + 1);
  }

  selectStage(id) {
    this.stageId = id;
    this.editor.clearHighlights();
    this.renderBar();
    this.renderStage();
  }

  renderBar() {
    this.bar.replaceChildren();
    this.def.stages.forEach((s, i) => {
      const status = this.result ? s.status(this.result) : 'idle';
      if (i > 0) this.bar.append(el('span', { class: 'arrow', text: '→' }));
      this.bar.append(el('button', {
        class: 'stage-btn ' + status + (s.id === this.stageId ? ' active' : ''),
        title: { ok: 'Stage succeeded', fail: 'Stage failed', skip: 'Not reached', idle: 'Not compiled yet' }[status],
        onclick: () => this.selectStage(s.id),
      }, el('span', { class: 'num', text: i + 1 }), s.label));
    });
  }

  renderStage() {
    const s = this.currentStage();
    const body = el('div', { class: 'stage-body' });
    const help = el('details', { class: 'stage-help', open: true }, el('summary', { text: 'What happens in this stage' }),
      [].concat(s.help).map((p) => el('p', { text: p })));
    this.panel.replaceChildren(el('h2', { text: s.label }), help, body);
    if (this.error) {
      body.append(note(this.error, 'error'));
    } else if (!this.result) {
      body.append(note('Press Compile (or Ctrl+Enter in the editor) to run the pipeline.'));
    } else if (s.status(this.result) === 'skip') {
      body.append(note('This stage was not reached because an earlier stage failed. Click the red stage in the pipeline bar to see the error.', 'warn'));
    } else {
      s.render(body, this.result, this);
    }
    this.panel.append(el('h3', { text: 'AI tutor' }), explainBox(this));
  }
}

// ---------------------------------------------------------------- startup
function parseHash() {
  const params = {};
  for (const pair of location.hash.replace(/^#/, '').split('&')) {
    const i = pair.indexOf('=');
    if (i > 0) params[decodeURIComponent(pair.slice(0, i))] = decodeURIComponent(pair.slice(i + 1));
  }
  return params;
}

function selectPart(id) {
  for (const b of document.querySelectorAll('#part-tabs .tab')) b.classList.toggle('active', b.dataset.part === id);
  for (const p of document.querySelectorAll('main > .part')) p.hidden = p.id !== 'part-' + id;
}

function renderHealth() {
  const h = App.health;
  const span = document.getElementById('health');
  if (!h) { span.textContent = 'server unreachable'; span.className = 'health fail'; return; }
  const gcc = (h.tools.gcc || 'gcc missing').replace(/^gcc \(.*?\) /, 'gcc ');
  const gem = h.gemini.configured ? 'Gemini: ' + (h.gemini.model || 'on') : 'Gemini: off';
  span.textContent = gcc + ' · minic ' + (h.tools.minic || 'not built') + ' · ' + gem;
  span.className = 'health ' + (h.tools.minic ? 'ok' : 'fail');
  span.title = Object.entries(h.tools).map(([k, v]) => k + ': ' + (v || 'MISSING')).join('\n');
}

// Deep links (also used for screenshots): #part=c&example=loops.c&stage=ir&opt=1&sub=elf
App.start = async function () {
  App.params = parseHash();
  try { App.health = await api('/api/health'); } catch (e) { showError(e.message); }
  renderHealth();
  for (const b of document.querySelectorAll('#part-tabs .tab')) b.addEventListener('click', () => selectPart(b.dataset.part));
  const active = App.params.part === 'lab' || App.parts[App.params.part] ? App.params.part : 'c';
  for (const [id, def] of Object.entries(App.parts)) {
    const ctrl = new PartController(document.getElementById('part-' + id), def);
    App.controllers[id] = ctrl;
    const mine = id === active;
    if (mine) ctrl.params = App.params;
    if (mine && App.params.stage && def.stages.some((s) => s.id === App.params.stage)) ctrl.stageId = App.params.stage;
    await ctrl.loadExamples(mine ? App.params.example : null);
    if (mine && ctrl.examples.length) await ctrl.compile();
  }
  try {
    App.lab = new CompilerLab(document.getElementById('part-lab'));
    await App.lab.start();
  } catch (e) { showError(e.message); }
  selectPart(active);
  document.body.dataset.ready = '1';
};
