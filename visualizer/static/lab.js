'use strict';

// One renderer for all protocol v1 compilers. Navigation replays recorded data.
class CompilerLab {
  constructor(root) {
    this.session = null;
    this.steps = [];
    this.cursor = 0;
    this.revision = 0;
    this.drafts = new Map();
    this.compiler = el('select', { 'aria-label': 'Compiler', onchange: () => this.changeCompiler() });
    this.opt = el('select', { 'aria-label': 'Optimization', onchange: () => this.invalidate() },
      el('option', { value: '0', text: 'O0' }), el('option', { value: '1', text: 'O1' }));
    this.button = el('button', { class: 'btn primary', text: 'Compile & inspect', onclick: () => this.compile() });
    this.message = el('div', { class: 'status-line', role: 'status', 'aria-live': 'polite' });
    this.description = note('');
    const left = el('div', { class: 'left' },
      el('div', { class: 'toolbar' }, this.compiler, this.opt, this.button), this.description);
    this.editor = new Editor(left);
    this.editor.onChange = () => this.invalidate();
    this.editor.textarea.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        if (!this.button.disabled) this.compile();
      }
    });
    left.append(this.message);
    this.bar = el('div', { class: 'pipeline-bar' });
    this.panel = el('div', { class: 'stage-panel' });
    root.append(note('Compiler Lab · Xem lại dữ liệu sau khi compile. Next/Back duyệt các bước đã ghi; không tạm dừng tiến trình compiler đang chạy.'),
      el('div', { class: 'part-grid' }, left, el('div', { class: 'right' }, this.bar, this.panel)));
    this.render();
  }

  async start() {
    const data = await api('/api/compilers');
    this.compilers = data.compilers;
    this.compiler.replaceChildren(...this.compilers.map(c => el('option', { value: c.id, text: c.name })));
    if (this.compilers.some(c => c.id === App.params.compiler)) this.compiler.value = App.params.compiler;
    this.changeCompiler();
  }

  changeCompiler() {
    if (this.currentCompiler) this.drafts.set(this.currentCompiler, this.editor.value);
    this.currentCompiler = this.compiler.value;
    const meta = this.compilers.find(c => c.id === this.currentCompiler);
    this.editor.value = this.drafts.get(this.currentCompiler) ?? meta.source ?? '';
    this.description.textContent = meta.description || '';
    this.invalidate();
  }

  invalidate() {
    this.revision++;
    this.session = null;
    this.steps = [];
    this.editor.clearHighlights();
    this.message.textContent = 'Nhấn Compile & inspect để tạo một phiên mới.';
    this.message.className = 'status-line';
    this.render();
  }

  async compile() {
    const revision = this.revision;
    this.button.disabled = this.compiler.disabled = this.opt.disabled = true;
    this.message.textContent = 'Compiling…';
    try {
      const session = await api('/api/compiler/compile', {
        compiler: this.compiler.value, source: this.editor.value, opt: Number(this.opt.value),
      });
      if (revision !== this.revision) return;
      this.session = session;
      this.steps = session.stages.flatMap((stage, si) => {
        const events = stage.events.length ? stage.events : [{ label: 'Kết quả pha', data: stage.output }];
        return events.map((event, ei) => ({ stage, si, event, ei }));
      });
      const failed = session.stages.find(s => s.status === 'fail');
      this.message.textContent = failed ? 'Lỗi tại: ' + failed.label : 'Compile hoàn tất. Dùng Next để khám phá từng bước.';
      this.message.className = 'status-line ' + (failed ? 'fail' : 'ok');
      const failIndex = this.steps.findIndex(step => step.stage === failed);
      this.select(failIndex >= 0 ? failIndex : 0);
    } catch (error) {
      if (revision !== this.revision) return;
      this.session = null;
      this.steps = [];
      this.message.textContent = error.message;
      this.message.className = 'status-line fail';
      this.render();
    } finally {
      this.button.disabled = this.compiler.disabled = this.opt.disabled = false;
    }
  }

  select(index) {
    this.cursor = Math.max(0, Math.min(index, this.steps.length - 1));
    this.editor.clearHighlights();
    const loc = this.steps[this.cursor]?.event.location;
    if (loc) {
      this.editor.setHighlights([loc]);
      this.editor.revealLine(loc.line);
    }
    this.render();
  }

  format(data) { return typeof data === 'string' ? data : JSON.stringify(data ?? null, null, 2); }

  render() {
    this.bar.replaceChildren();
    if (!this.session) {
      this.panel.replaceChildren(el('h2', { text: 'Compiler Lab' }),
        note('Chọn compiler, sửa source và compile. Giao diện này đọc cùng một protocol cho mọi adapter.'),
        note('Mini C: token → AST → symbols → IR → optimization → assembly. Expression compiler: thử 2 + 3 * 4 để xem độ ưu tiên và constant folding.'));
      return;
    }
    const step = this.steps[this.cursor];
    const stage = step.stage;
    this.session.stages.forEach((s, si) => this.bar.append(el('button', {
      class: 'stage-btn ' + s.status + (s === stage ? ' active' : ''),
      'aria-pressed': s === stage ? 'true' : 'false',
      text: (si + 1) + '. ' + s.label + (s.status === 'skip' ? ' (skip)' : s.status === 'fail' ? ' (error)' : ''),
      onclick: () => this.select(this.steps.findIndex(item => item.si === si)),
    })));
    const previous = el('button', { class: 'btn', text: '← Back', disabled: this.cursor === 0,
      onclick: () => this.select(this.cursor - 1) });
    const next = el('button', { class: 'btn', text: 'Next →', disabled: this.cursor === this.steps.length - 1,
      onclick: () => this.select(this.cursor + 1) });
    const slider = el('input', { type: 'range', min: '0', max: String(this.steps.length - 1),
      value: String(this.cursor), 'aria-label': 'Trace step', oninput: event => this.select(Number(event.target.value)) });
    const list = el('div', { class: 'lab-events', 'aria-label': 'Stage steps' });
    // Window long traces to keep the page responsive even for large token streams.
    const start = Math.max(0, step.ei - 30);
    stage.events.slice(start, step.ei + 31).forEach((event, offset) => list.append(el('button', {
      class: 'lab-event' + (start + offset === step.ei ? ' active' : ''),
      'aria-current': start + offset === step.ei ? 'step' : null,
      text: (start + offset + 1) + '. ' + event.label,
      onclick: () => this.select(this.cursor - step.ei + start + offset),
    })));
    this.panel.replaceChildren(...[el('h2', { text: stage.label }), note(stage.help),
      el('div', { class: 'toolbar' }, previous, next,
        el('span', { text: 'Bước ' + (this.cursor + 1) + ' / ' + this.steps.length }), slider),
      stage.status === 'skip' ? note('Pha này không chạy trong phiên compile này.', 'warn') : null,
      el('h3', { text: step.event.label }),
      step.event.location ? note('Source: ' + step.event.location.line + ':' + (step.event.location.col || 1)) : null,
      codeBlock(this.format(step.event.data)), list,
      el('details', { class: 'stage-help' }, el('summary', { text: 'Toàn bộ kết quả pha (snapshot sau compile)' }),
        codeBlock(this.format(stage.output)))].filter(node => node != null));
  }
}
