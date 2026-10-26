// liveedit.js (v2.0.0, phase 1.6): the editor window
//
// index.html calls liveEdit.canOpen(name) and liveEdit.open(name, path) when a
// file is clicked. this file only builds the window (header, history, close
// guard, error messages). the real viewers/editors (access, office, csv, pdf)
// come in later phases and plug in with liveEdit.register(types, mount)

(function () {
  'use strict';

  // every type the window opens. images and video keep the old preview
  const TYPES = ['xls', 'xlsx', 'doc', 'docx', 'ppt', 'pptx',
    'odt', 'ods', 'odp', 'rtf', 'csv', 'tsv', 'txt', 'pdf', 'sqlite', 'db'];

  const enc = encodeURIComponent;

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text; // textContent only, file names are never html
    return e;
  };

  const extOf = n => (n.match(/\.([^.]+)$/) || [, ''])[1].toLowerCase();
  const dlUrl = p => '/api/download?name=' + enc(p);
  const timeText = ms => new Date(ms).toLocaleString();

  function sizeText(b) {
    if (b < 1024) return b + ' B';
    const units = ['KB', 'MB', 'GB'];
    let i = -1;
    do { b /= 1024; i++; } while (b >= 1024 && i < units.length - 1);
    return b.toFixed(b < 10 ? 1 : 0) + ' ' + units[i];
  }

  // -- talking to the server --

  // what the server's answers mean in plain words. every failure has to be
  // visible to the user (plan rule R9), never a silent nothing
  const REASONS = {
    400: 'The request was not valid.',
    403: 'That path is not allowed.',
    404: 'The file was not found. It may have been moved or deleted.',
    405: 'This kind of file can only be viewed, not changed.',
    409: 'The file changed on the server since you opened it. Close it and open it again.',
    413: 'The file is too large.',
    415: 'The new content is a different kind of file, so nothing was saved.',
    423: 'The file is open in another program, so it cannot be changed right now.'
  };

  function explain(r) {
    if (r.status === 0) return r.text;
    // the server's own sentence for failed saves ("could not back up...")
    if (r.status === 500 && r.text && r.text.length < 200 && !r.json) return r.text;
    // the access routes answer with {error, message}: busy, timeout, validation...
    // those sentences say more than the generic table below
    const m = r.json && typeof r.json.message === 'string' ? r.json.message : '';
    if (m && [422, 500, 503, 504].includes(r.status)) return m.charAt(0).toUpperCase() + m.slice(1) + '.';
    return REASONS[r.status] || 'Something went wrong (error ' + r.status + ').';
  }

  async function request(url, opts) {
    let r;
    try {
      r = await fetch(url, opts);
    } catch {
      return { ok: false, status: 0, text: 'Cannot reach the server.', json: null };
    }
    const text = await r.text();
    let json = null;
    try { json = JSON.parse(text); } catch { /* plain text answer */ }
    return { ok: r.ok, status: r.status, text, json };
  }

  // -- the window --

  const handlers = []; // { types, mount } added by later phases
  const ui = {};
  let cur = null;      // the open file: { name, path, ext, info, dirty }
  let noteTimer;

  // the page's own toast (say) is hidden behind a modal dialog, so while the
  // window is open messages go into its own status line instead
  function notify(text, bad) {
    if (!ui.dlg || !ui.dlg.open) {
      if (typeof say === 'function') say(text, bad);
      return;
    }
    clearTimeout(noteTimer);
    ui.status.textContent = text;
    ui.status.className = 'le-status' + (bad ? ' bad' : '');
    ui.status.hidden = !text;
    if (text) noteTimer = setTimeout(() => { ui.status.hidden = true; }, bad ? 8000 : 3500);
  }

  function setDirty(v) { if (cur) cur.dirty = !!v; }

  // closing with edits that aren't saved yet asks first
  function requestClose() {
    if (cur && cur.dirty && !confirm('You have changes that are not saved. Close without saving?')) return;
    ui.dlg.close();
  }

  window.addEventListener('beforeunload', e => {
    if (cur && cur.dirty) { e.preventDefault(); e.returnValue = ''; }
  });

  function build() {
    if (ui.dlg) return;

    ui.dlg = el('dialog');
    ui.dlg.id = 'ledlg';

    ui.title = el('span', 'le-title');
    ui.histBtn = el('button', '', 'History');
    ui.closeBtn = el('button', 'primary', 'Close');
    const head = el('div', 'le-head');
    head.append(ui.title, ui.histBtn, ui.closeBtn);

    ui.banner = el('div', 'le-banner');
    ui.banner.hidden = true;

    ui.stage = el('div', 'le-stage');
    ui.body = el('div', 'le-body');
    ui.hist = el('div', 'le-hist');
    ui.hist.hidden = true;
    ui.stage.append(ui.body, ui.hist);

    ui.status = el('div', 'le-status');
    ui.status.hidden = true;

    ui.actions = el('span', 'le-actions'); // buttons of the editor that is open
    ui.dl = el('a', 'btn', 'Download');
    ui.foot = el('div', 'le-foot');
    ui.foot.append(ui.actions, ui.dl);

    ui.dlg.append(head, ui.banner, ui.stage, ui.status, ui.foot);
    document.body.append(ui.dlg);

    ui.closeBtn.onclick = requestClose;
    ui.histBtn.onclick = toggleHistory;
    // esc and the android back button fire "cancel": run it through the guard
    ui.dlg.addEventListener('cancel', e => { e.preventDefault(); requestClose(); });
    ui.dlg.addEventListener('close', () => {
      cur = null;
      ui.body.textContent = '';
      ui.actions.textContent = '';
      ui.hist.hidden = true;
      ui.histBtn.textContent = 'History';
      ui.banner.hidden = true;
      ui.status.hidden = true;
    });
  }

  // what a viewer/editor gets when it is mounted
  function makeCtx(me) {
    return {
      name: me.name,
      path: me.path,
      ext: me.ext,
      get info() { return me.info; }, // size, version token, lockedByOtherApp...
      body: ui.body,                  // fill this
      actions: ui.actions,            // put your buttons (Edit, Save changes...) here
      setDirty,
      notify,
      api: { request, explain, enc },
      // after a save: remember the new version token, the next save needs it
      setVersion(v) { me.info.version = v; },
      // read the file info again from the server
      async refreshInfo() {
        const r = await request('/api/fileinfo?name=' + enc(me.path));
        if (r.ok && r.json && cur === me) me.info = r.json;
        return me.info;
      }
    };
  }

  // shown for every type that has no real viewer yet
  const fallback = {
    mount(ctx) {
      const i = ctx.info, card = el('div', 'le-card');
      card.append(
        el('div', 'le-ext', ctx.ext || 'file'),
        el('h2', '', ctx.name),
        el('p', 'le-meta', sizeText(i.size) + ' · changed ' + timeText(i.mtimeMs)),
        el('p', 'le-msg', 'There is no viewer for this kind of file yet. You can still download it, or look at its history.'));
      ctx.body.append(card);
    }
  };

  function showProblem(text) {
    ui.body.textContent = '';
    ui.body.append(el('p', 'le-msg bad', text));
    notify(text, true);
  }

  // loads the file info and starts the viewer. also used again after a restore
  async function mount() {
    const me = cur;
    const r = await request('/api/fileinfo?name=' + enc(me.path));
    if (cur !== me) return; // closed while loading
    if (!r.ok || !r.json) { showProblem(explain(r)); return; }

    me.info = r.json;
    me.dirty = false;
    ui.banner.hidden = !r.json.lockedByOtherApp;
    ui.banner.textContent = 'This database is open in Access on another computer. You can look at it, but changes cannot be saved until it is closed there.';
    ui.body.textContent = '';
    ui.actions.textContent = '';

    const h = handlers.find(x => x.types.includes(me.ext)) || fallback;
    try {
      await h.mount(makeCtx(me));
    } catch (e) {
      console.error('[liveedit]', e);
      if (cur === me) showProblem('This file could not be opened: ' + e.message);
    }
  }

  async function open(name, path) {
    build();
    if (ui.dlg.open) return;
    cur = { name, path, ext: extOf(name), info: null, dirty: false };
    ui.title.textContent = name;
    ui.dl.href = dlUrl(path);
    ui.dl.download = name;
    ui.body.textContent = '';
    ui.body.append(el('p', 'le-msg', 'Opening…'));
    ui.actions.textContent = '';
    ui.hist.hidden = true;
    ui.banner.hidden = true;
    ui.dlg.showModal();
    await mount();
  }

  // -- history (every old version the server kept) --

  const WHY = {
    'save': 'before a save',
    'office': 'before an office save',
    'upload-overwrite': 'before an upload replaced it',
    'pre-restore': 'before a restore'
  };

  async function toggleHistory() {
    if (!cur) return;
    if (!ui.hist.hidden) {
      ui.hist.hidden = true;
      ui.histBtn.textContent = 'History';
      return;
    }
    ui.hist.hidden = false;
    ui.histBtn.textContent = 'Back';
    await renderHistory();
  }

  async function renderHistory() {
    const me = cur;
    ui.hist.textContent = '';
    ui.hist.append(el('p', 'le-msg', 'Loading…'));
    const r = await request('/api/versions?name=' + enc(me.path));
    if (cur !== me) return;

    ui.hist.textContent = '';
    if (!r.ok || !Array.isArray(r.json)) { ui.hist.append(el('p', 'le-msg bad', explain(r))); return; }

    ui.hist.append(el('h2', '', 'Earlier versions'));
    if (!r.json.length) {
      ui.hist.append(el('p', 'le-msg', 'No earlier versions yet. One is kept every time this file is changed.'));
      return;
    }

    const list = el('ul', 'le-list');
    for (const v of r.json) {
      const li = el('li', 'le-item'), txt = el('div', 'le-iinfo');
      txt.append(
        el('b', '', timeText(v.savedAt * 1000)),
        el('small', '', sizeText(v.size) + ' · ' + (WHY[v.reason] || v.reason)));
      const dl = el('a', 'btn', 'Download');
      dl.href = '/api/versions/download?name=' + enc(me.path) + '&id=' + enc(v.id);
      dl.download = me.name;
      const rs = el('button', '', 'Restore');
      rs.onclick = () => restore(v);
      li.append(txt, dl, rs);
      list.append(li);
    }
    ui.hist.append(list);
  }

  async function restore(v) {
    const me = cur;
    let msg = 'Put back the version from ' + timeText(v.savedAt * 1000) +
      '? The file as it is now is kept in the history too.';
    if (me.dirty) msg += '\n\nYour changes that are not saved will be lost.';
    if (!confirm(msg)) return;

    // the token we loaded: if someone changed the file meanwhile, the server says 409
    const r = await request('/api/versions/restore?name=' + enc(me.path) + '&id=' + enc(v.id) +
      '&expected=' + enc(me.info.version), { method: 'POST' });
    if (cur !== me) return;
    if (!r.ok) { notify(explain(r), true); return; }

    ui.hist.hidden = true;
    ui.histBtn.textContent = 'History';
    notify('Version restored.');
    await mount(); // show the file as it is now
  }

  // -- what index.html uses --

  window.liveEdit = {
    canOpen: name => TYPES.includes(extOf(name)),
    open,
    // later phases: liveEdit.register(['csv', 'tsv'], ctx => { ... })
    register(types, mount) {
      handlers.push({ types: types.map(t => t.toLowerCase()), mount });
    }
  };

  // ── Generic SQLite viewer ─────────────────────────────────────────────────
  // Opens any .sqlite / .db file in the drive.
  // Shows a table-selector dropdown, paginated rows in a plain grid,
  // per-column search filters, and tap-to-edit cells.

  async function mountGenericDb(ctx) {
    const { request, explain } = ctx.api;
    const root = document.createElement('div');
    root.className = 'gdb-root';

    // ── state ──
    const st = {
      schema:    null,   // { tables: [{name, columns:[{name,type,pk}]}] }
      table:     '',     // currently selected table name
      pkCol:     '',     // first PK column (used for saves)
      columns:   [],     // [{name,type,pk}] for current table
      rows:      [],     // current page rows
      total:     0,
      page:      1,
      limit:     100,
      order:     '',
      dir:       'asc',
      filters:   {},     // col -> value
      token:     '',
      saving:    false,
    };

    // ── tiny helpers ──
    const gdbEl  = (tag, cls, text) => {
      const e = document.createElement(tag);
      if (cls)  e.className   = cls;
      if (text != null) e.textContent = text;
      return e;
    };
    const gdbEnc = encodeURIComponent;

    function gdbMsg(text, bad) {
      ctx.notify(text, bad);
    }

    // ── load schema ──
    async function loadSchema() {
      root.textContent = '';
      root.append(gdbEl('p', 'le-msg', 'Loading schema…'));
      const r = await request('/api/db/schema?name=' + gdbEnc(ctx.path));
      if (!r.ok) {
        root.textContent = '';
        root.append(gdbEl('p', 'le-msg bad', 'Could not load schema: ' + explain(r)));
        return;
      }
      st.schema = r.json;
      st.token  = r.json.version || '';
      root.textContent = '';
      buildUI();
    }

    // ── build top-level UI ──
    function buildUI() {
      const tables = (st.schema && st.schema.tables) || [];

      // table selector
      const bar = gdbEl('div', 'gdb-bar');
      const lbl = gdbEl('label', 'gdb-table-lbl', 'Table: ');
      const sel = gdbEl('select', 'gdb-table-sel');
      tables.forEach(t => {
        const opt = document.createElement('option');
        opt.value = t.name;
        opt.textContent = t.name;
        sel.append(opt);
      });
      lbl.append(sel);
      bar.append(lbl);

      // page-size selector
      const psLbl = gdbEl('label', 'gdb-ps-lbl', ' Rows/page: ');
      const psSel = gdbEl('select', 'gdb-ps-sel');
      [25, 50, 100, 250].forEach(n => {
        const opt = document.createElement('option');
        opt.value = n;
        opt.textContent = n;
        if (n === st.limit) opt.selected = true;
        psSel.append(opt);
      });
      psLbl.append(psSel);
      bar.append(psLbl);

      root.append(bar);

      // grid container
      const gridWrap = gdbEl('div', 'gdb-grid-wrap');
      root.append(gridWrap);

      // pagination bar
      const pageBar = gdbEl('div', 'gdb-page-bar');
      root.append(pageBar);

      ctx.body.append(root);

      function switchTable(name) {
        st.table   = name;
        st.page    = 1;
        st.filters = {};
        st.order   = '';
        st.dir     = 'asc';
        const tdef = tables.find(t => t.name === name);
        st.columns = tdef ? tdef.columns : [];
        const pk   = st.columns.find(c => c.pk);
        st.pkCol   = pk ? pk.name : (st.columns[0] ? st.columns[0].name : 'rowid');
        loadRows(gridWrap, pageBar);
      }

      sel.onchange = () => switchTable(sel.value);
      psSel.onchange = () => {
        st.limit = parseInt(psSel.value, 10);
        st.page  = 1;
        loadRows(gridWrap, pageBar);
      };

      if (tables.length > 0) switchTable(tables[0].name);
    }

    // ── load rows for current table/page ──
    async function loadRows(gridWrap, pageBar) {
      gridWrap.textContent = '';
      gridWrap.append(gdbEl('p', 'le-msg', 'Loading…'));

      let url = '/api/db/generic/rows?name=' + gdbEnc(ctx.path)
              + '&table=' + gdbEnc(st.table)
              + '&page='  + st.page
              + '&limit=' + st.limit;
      if (st.order) url += '&order=' + gdbEnc(st.order) + '&dir=' + st.dir;
      Object.entries(st.filters).forEach(([col, val]) => {
        if (val) url += '&filter_' + gdbEnc(col) + '=' + gdbEnc(val);
      });

      const r = await request(url);
      if (!r.ok) {
        gridWrap.textContent = '';
        gridWrap.append(gdbEl('p', 'le-msg bad', 'Error: ' + explain(r)));
        return;
      }

      st.rows  = r.json.rows  || [];
      st.total = r.json.total || 0;
      if (r.json.version) st.token = String(r.json.version);

      renderGrid(gridWrap);
      renderPageBar(pageBar, gridWrap);
    }

    // ── render grid ──
    function renderGrid(gridWrap) {
      gridWrap.textContent = '';
      if (!st.columns.length) {
        gridWrap.append(gdbEl('p', 'le-msg', 'No columns found.'));
        return;
      }

      const wrap = gdbEl('div', 'gdb-table-wrap');
      const table = gdbEl('table', 'gdb-table');

      // header row + sort controls
      const thead = gdbEl('thead');
      const htr   = gdbEl('tr');
      st.columns.forEach(c => {
        const th = gdbEl('th', 'gdb-th');
        const nameSpan = gdbEl('span', 'gdb-th-name', c.name);
        nameSpan.title = c.type || '';
        if (c.pk) nameSpan.classList.add('gdb-pk');
        nameSpan.style.cursor = 'pointer';
        nameSpan.onclick = () => {
          if (st.order === c.name) {
            st.dir = st.dir === 'asc' ? 'desc' : 'asc';
          } else {
            st.order = c.name;
            st.dir   = 'asc';
          }
          st.page = 1;
          loadRows(gridWrap, gridWrap.nextElementSibling);
        };
        if (st.order === c.name) {
          nameSpan.textContent += st.dir === 'asc' ? ' ▲' : ' ▼';
        }
        th.append(nameSpan);
        htr.append(th);
      });
      thead.append(htr);

      // filter row
      const ftr = gdbEl('tr');
      st.columns.forEach(c => {
        const td  = gdbEl('td', 'gdb-td gdb-filter-td');
        const inp = gdbEl('input');
        inp.type        = 'text';
        inp.className   = 'gdb-filter-inp';
        inp.placeholder = 'Filter…';
        inp.value       = st.filters[c.name] || '';
        let debounce;
        inp.oninput = () => {
          clearTimeout(debounce);
          debounce = setTimeout(() => {
            st.filters[c.name] = inp.value.trim();
            st.page = 1;
            loadRows(gridWrap, gridWrap.nextElementSibling);
          }, 300);
        };
        td.append(inp);
        ftr.append(td);
      });
      thead.append(ftr);
      table.append(thead);

      // body
      const tbody = gdbEl('tbody');
      if (!st.rows.length) {
        const tr = gdbEl('tr');
        const td = gdbEl('td', 'gdb-td gdb-empty');
        td.colSpan = st.columns.length;
        td.textContent = 'No rows.';
        tr.append(td);
        tbody.append(tr);
      } else {
        st.rows.forEach((row, ri) => {
          const tr = gdbEl('tr', 'gdb-tr');
          st.columns.forEach(c => {
            const val = row[c.name];
            const td  = gdbEl('td', 'gdb-td');
            td.textContent = val == null ? '' : String(val);
            td.title = val == null ? '(null)' : String(val);

            // tap/click to edit — PKs are not editable
            if (!c.pk) {
              td.classList.add('gdb-editable');
              td.onclick = () => startEdit(td, row, c, ri);
            }
            tr.append(td);
          });
          tbody.append(tr);
        });
      }
      table.append(tbody);
      wrap.append(table);
      gridWrap.append(wrap);
    }

    // ── inline cell editing ──
    function startEdit(td, row, colDef, _ri) {
      if (td.querySelector('input, select')) return; // already editing
      const oldText = td.textContent;
      const oldVal  = row[colDef.name];

      td.textContent = '';
      const inp = gdbEl('input');
      inp.type      = 'text';
      inp.className = 'gdb-cell-inp';
      inp.value     = oldVal == null ? '' : String(oldVal);
      td.append(inp);
      inp.focus();
      inp.select();

      async function commitEdit() {
        const newVal = inp.value;
        if (newVal === (oldVal == null ? '' : String(oldVal))) {
          // no change
          td.textContent = oldText;
          return;
        }
        if (st.saving) { td.textContent = oldText; return; }
        st.saving = true;
        inp.disabled = true;

        const pkVal = row[st.pkCol];
        const body  = {
          path:   ctx.path,
          table:  st.table,
          pkCol:  st.pkCol,
          pkVal:  pkVal,
          column: colDef.name,
          old:    oldVal,
          new:    newVal === '' ? null : newVal,
          token:  st.token,
        };

        const r = await request('/api/db/generic/save', {
          method:  'POST',
          headers: { 'Content-Type': 'application/json' },
          body:    JSON.stringify(body),
        });

        st.saving = false;

        if (r.ok && r.json && r.json.version) {
          st.token = String(r.json.version);
          row[colDef.name] = newVal === '' ? null : newVal;
          td.textContent   = newVal;
          gdbMsg('Saved.');
        } else if (r.status === 409) {
          td.textContent = oldText;
          gdbMsg('Conflict: the database changed. Reloading…', true);
          setTimeout(() => loadRows(td.closest('.gdb-grid-wrap'), td.closest('.gdb-grid-wrap').nextElementSibling), 800);
        } else {
          td.textContent = oldText;
          gdbMsg('Save failed: ' + explain(r), true);
        }
      }

      inp.onblur  = commitEdit;
      inp.onkeydown = e => {
        if (e.key === 'Enter')  { inp.blur(); }
        if (e.key === 'Escape') { td.textContent = oldText; inp.onblur = null; }
      };
    }

    // ── pagination bar ──
    function renderPageBar(pageBar, gridWrap) {
      pageBar.textContent = '';
      const totalPages = Math.max(1, Math.ceil(st.total / st.limit));
      const info = gdbEl('span', 'gdb-page-info',
        'Rows ' + ((st.page - 1) * st.limit + 1) + '–' +
        Math.min(st.page * st.limit, st.total) + ' of ' + st.total);

      const prev = gdbEl('button', 'btn gdb-page-btn', '← Prev');
      prev.disabled = st.page <= 1;
      prev.onclick  = () => { st.page--; loadRows(gridWrap, pageBar); };

      const next = gdbEl('button', 'btn gdb-page-btn', 'Next →');
      next.disabled = st.page >= totalPages;
      next.onclick  = () => { st.page++; loadRows(gridWrap, pageBar); };

      const pageNum = gdbEl('span', 'gdb-page-num',
        'Page ' + st.page + ' / ' + totalPages);

      pageBar.append(prev, pageNum, info, next);
    }

    // ── kick off ──
    await loadSchema();
  }

  // Dispatcher: custom handler if registered, otherwise generic SQLite viewer
  window.liveEdit.register(['sqlite', 'db'], function (ctx) {
    if (window.customDbHandler && window.customDbHandler.canHandle(ctx.name)) {
      return window.customDbHandler.mount(ctx);
    }
    return mountGenericDb(ctx);
  });

  // ── Phase 4: shared encoding / CSV utilities ──────────────────────────────

  // Detect UTF-8 (with/without BOM) vs Windows-1250.
  function p4DetectEncoding(bytes) {
    if (bytes.length >= 3 &&
        bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF) {
      return { enc: 'utf-8', bom: true };
    }
    try {
      new TextDecoder('utf-8', { fatal: true }).decode(bytes);
      return { enc: 'utf-8', bom: false };
    } catch {
      return { enc: 'windows-1250', bom: false };
    }
  }

  // Decode bytes to a JS string using the detected encoding, stripping BOM.
  function p4Decode(bytes, info) {
    return new TextDecoder(info.enc).decode(
      info.bom ? bytes.slice(3) : bytes
    );
  }

  // Build char→byte lookup for Windows-1250 re-encoding.
  function p4BuildWin1250Map() {
    const map = new Map();
    const dec = new TextDecoder('windows-1250');
    for (let i = 0; i < 256; i++) {
      const ch = dec.decode(new Uint8Array([i]));
      if (!map.has(ch)) map.set(ch, i);
    }
    return map;
  }

  // Encode a JS string back to bytes.
  // Returns { bytes: Uint8Array, err: null } or { bytes: null, err: '...' }.
  function p4Encode(str, info, win1250Map) {
    if (info.enc === 'utf-8') {
      const body = new TextEncoder().encode(str);
      if (!info.bom) return { bytes: body, err: null };
      const out = new Uint8Array(3 + body.length);
      out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF;
      out.set(body, 3);
      return { bytes: out, err: null };
    }
    // windows-1250
    const bytes = new Uint8Array(str.length);
    for (let i = 0; i < str.length; i++) {
      const b = win1250Map.get(str[i]);
      if (b === undefined) {
        return { bytes: null, err: 'Caracterul "' + str[i] + '" nu poate fi salvat în Windows-1250.' };
      }
      bytes[i] = b;
    }
    return { bytes, err: null };
  }

  // Detect CRLF vs LF.
  function p4DetectLE(text) {
    return text.includes('\r\n') ? '\r\n' : '\n';
  }

  // Detect delimiter by scoring comma, semicolon and tab across the first few lines.
  function p4DetectDelim(lines) {
    const cands = [',', ';', '\t'];
    const best = cands.map(d => {
      const counts = lines.slice(0, 6).map(l => {
        let n = 0; for (const c of l) if (c === d) n++; return n;
      });
      const avg = counts.reduce((a, b) => a + b, 0) / (counts.length || 1);
      const variance = counts.reduce((a, b) => a + (b - avg) ** 2, 0) / (counts.length || 1);
      return { d, score: avg - variance * 0.5 };
    });
    best.sort((a, b) => b.score - a.score);
    return best[0].score > 0 ? best[0].d : ',';
  }

  // RFC 4180 parser — handles quoted cells (embedded delimiters, newlines, escaped quotes).
  function p4ParseCSV(text, delim) {
    const rows = [];
    let i = 0;
    const n = text.length;
    while (i < n) {
      const row = [];
      // eslint-disable-next-line no-constant-condition
      while (true) {
        if (i < n && text[i] === '"') {
          i++; // opening quote
          let field = '';
          while (i < n) {
            if (text[i] === '"') {
              if (i + 1 < n && text[i + 1] === '"') { field += '"'; i += 2; }
              else { i++; break; }
            } else { field += text[i++]; }
          }
          row.push(field);
        } else {
          let field = '';
          while (i < n && text[i] !== delim && text[i] !== '\r' && text[i] !== '\n')
            field += text[i++];
          row.push(field);
        }
        if (i < n && text[i] === delim) { i++; continue; }
        break;
      }
      if (i < n && text[i] === '\r') i++;
      if (i < n && text[i] === '\n') i++;
      rows.push(row);
    }
    return rows;
  }

  // RFC 4180 serialiser.
  function p4SerializeCSV(rows, delim, lineEnd) {
    return rows.map(row =>
      row.map(cell => {
        const s = String(cell == null ? '' : cell);
        if (s.includes(delim) || s.includes('"') || s.includes('\n') || s.includes('\r'))
          return '"' + s.replace(/"/g, '""') + '"';
        return s;
      }).join(delim)
    ).join(lineEnd);
  }

  // Fetch a file as raw bytes (needed for encoding-safe handling).
  async function p4FetchBinary(url) {
    try {
      const r = await fetch(url);
      if (!r.ok) {
        const text = await r.text();
        return { ok: false, status: r.status, text };
      }
      return { ok: true, status: r.status, bytes: new Uint8Array(await r.arrayBuffer()) };
    } catch {
      return { ok: false, status: 0, text: 'Cannot reach the server.' };
    }
  }

  // Save bytes back to the server.
  async function p4Save(path, bytes, expectedVersion) {
    try {
      const r = await fetch(
        '/api/save?name=' + enc(path) + '&expected=' + enc(expectedVersion),
        { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: bytes }
      );
      const text = await r.text();
      let json = null;
      try { json = JSON.parse(text); } catch {}
      return { ok: r.ok, status: r.status, text, json };
    } catch {
      return { ok: false, status: 0, text: 'Cannot reach the server.', json: null };
    }
  }

  // Extract the new version token from a save response.
  function p4Version(r) {
    if (r.json && r.json.version != null) return String(r.json.version);
    const t = (r.text || '').trim();
    return (!t.startsWith('{') && t.length < 200) ? t : '';
  }

  // ── CSV / TSV live editor ─────────────────────────────────────────────────

  async function mountCsv(ctx) {
    const { explain } = ctx.api;
    const MAX_RW = 20 * 1024 * 1024; // 20 MB read-only threshold

    ctx.body.append(el('p', 'le-msg', 'Se încarcă…'));

    const dl = await p4FetchBinary('/api/download?name=' + enc(ctx.path));
    ctx.body.textContent = '';
    if (!dl.ok) { ctx.body.append(el('p', 'le-msg bad', dl.text)); return; }

    const encInfo   = p4DetectEncoding(dl.bytes);
    const raw       = p4Decode(dl.bytes, encInfo);
    const lineEnd   = p4DetectLE(raw);
    const hasTrail  = raw.endsWith('\n');
    // strip trailing newline before parsing so we don't get a phantom empty row
    const stripped  = hasTrail ? raw.slice(0, raw.lastIndexOf('\n') + 1 - (raw.endsWith('\r\n') ? 2 : 1)) : raw;
    const preLines  = stripped.split(/\r?\n/);
    const delim     = p4DetectDelim(preLines);
    let   rows      = p4ParseCSV(stripped, delim);
    const readOnly  = dl.bytes.length > MAX_RW;
    const win1250Map = encInfo.enc === 'windows-1250' ? p4BuildWin1250Map() : null;

    if (readOnly) {
      ctx.body.append(el('p', 'le-msg bad',
        '⚠ Fișierul depășește 20 MB — deschis doar pentru citire.'));
    }

    function colCount() { return rows.reduce((m, r) => Math.max(m, r.length), 0); }

    const root      = el('div', 'le-csv-root');
    const tableWrap = el('div', 'le-csv-wrap');
    const table     = el('table', 'le-csv-table');
    tableWrap.append(table);
    root.append(tableWrap);

    function makeCell(ri, ci, initVal) {
      const td   = el('td', 'le-csv-td');
      const span = el('span', 'le-csv-cell-text');
      span.textContent = initVal;
      td.append(span);
      if (readOnly) return td;

      td.title = 'Apasă pentru a edita';
      td.onclick = () => {
        if (td.querySelector('input')) return;
        span.hidden = true;
        const inp = el('input', 'le-csv-inp');
        inp.value = rows[ri][ci] != null ? rows[ri][ci] : '';
        inp.autocomplete = 'off';
        td.append(inp);
        inp.focus(); inp.select();

        const commit = () => {
          const v = inp.value;
          while (rows[ri].length <= ci) rows[ri].push('');
          rows[ri][ci] = v;
          span.textContent = v;
          span.hidden = false;
          inp.remove();
          ctx.setDirty(true);
        };
        inp.addEventListener('blur', commit);
        inp.addEventListener('keydown', e => {
          if (e.key === 'Enter') { e.preventDefault(); commit(); }
          if (e.key === 'Tab')   { e.preventDefault(); commit(); }
          if (e.key === 'Escape') { span.hidden = false; inp.remove(); }
        });
      };
      return td;
    }

    function renderTable() {
      table.textContent = '';
      const cols = colCount();

      // row 0 → sticky header
      if (rows.length > 0) {
        const thead = el('thead');
        const htr   = el('tr');
        htr.append(el('th', 'le-csv-th le-csv-rnum', '#'));
        const headerRow = rows[0];
        for (let c = 0; c < cols; c++) {
          const th = el('th', 'le-csv-th');
          th.textContent = headerRow[c] != null ? headerRow[c] : '';
          htr.append(th);
        }
        if (!readOnly) htr.append(el('th', 'le-csv-th le-csv-del-hd', ''));
        thead.append(htr);
        table.append(thead);
      }

      const tbody = el('tbody');
      for (let ri = 1; ri < rows.length; ri++) {
        const tr = el('tr', 'le-csv-tr');
        const numTd = el('td', 'le-csv-td le-csv-rnum');
        numTd.textContent = ri;
        tr.append(numTd);
        for (let ci = 0; ci < cols; ci++) {
          tr.append(makeCell(ri, ci, rows[ri][ci] != null ? rows[ri][ci] : ''));
        }
        if (!readOnly) {
          const delTd  = el('td', 'le-csv-td le-csv-del-td');
          const delBtn = el('button', 'le-csv-del-btn', '✕');
          delBtn.title = 'Șterge rândul';
          // capture ri in closure via let
          const capturedRi = ri;
          delBtn.onclick = () => {
            rows.splice(capturedRi, 1);
            ctx.setDirty(true);
            renderTable();
          };
          delTd.append(delBtn);
          tr.append(delTd);
        }
        tbody.append(tr);
      }
      table.append(tbody);
    }

    renderTable();
    ctx.body.append(root);

    if (!readOnly) {
      const addRowBtn = el('button', 'btn le-csv-add-row', '+ Rând nou');
      addRowBtn.onclick = () => {
        rows.push(Array(colCount()).fill(''));
        ctx.setDirty(true);
        renderTable();
      };

      const saveBtn = el('button', 'btn primary le-csv-save', 'Salvează');
      saveBtn.onclick = async () => {
        saveBtn.disabled = true;
        saveBtn.textContent = 'Se salvează…';

        const trail = hasTrail ? lineEnd : '';
        const text  = p4SerializeCSV(rows, delim, lineEnd) + trail;
        const { bytes, err } = p4Encode(text, encInfo, win1250Map);
        if (err) {
          ctx.notify(err, true);
          saveBtn.disabled = false;
          saveBtn.textContent = 'Salvează';
          return;
        }

        const r = await p4Save(ctx.path, bytes, ctx.info.version);
        saveBtn.disabled = false;
        saveBtn.textContent = 'Salvează';
        if (r.ok) {
          const v = p4Version(r);
          if (v) ctx.setVersion(v);
          ctx.setDirty(false);
          ctx.notify('Salvat.');
        } else {
          ctx.notify(explain(r), true);
        }
      };

      ctx.actions.append(addRowBtn, saveBtn);
    }
  }

  // ── Plain text editor ─────────────────────────────────────────────────────

  async function mountTxt(ctx) {
    const { explain } = ctx.api;

    ctx.body.append(el('p', 'le-msg', 'Se încarcă…'));

    const dl = await p4FetchBinary('/api/download?name=' + enc(ctx.path));
    ctx.body.textContent = '';
    if (!dl.ok) { ctx.body.append(el('p', 'le-msg bad', dl.text)); return; }

    const encInfo    = p4DetectEncoding(dl.bytes);
    const text       = p4Decode(dl.bytes, encInfo);
    const win1250Map = encInfo.enc === 'windows-1250' ? p4BuildWin1250Map() : null;

    const area = document.createElement('textarea');
    area.className    = 'le-txt-area';
    area.value        = text;
    area.spellcheck   = false;
    area.autocomplete = 'off';
    area.addEventListener('input', () => ctx.setDirty(true));
    ctx.body.append(area);

    const saveBtn = el('button', 'btn primary', 'Salvează');
    saveBtn.onclick = async () => {
      saveBtn.disabled    = true;
      saveBtn.textContent = 'Se salvează…';

      const { bytes, err } = p4Encode(area.value, encInfo, win1250Map);
      if (err) {
        ctx.notify(err, true);
        saveBtn.disabled    = false;
        saveBtn.textContent = 'Salvează';
        return;
      }

      const r = await p4Save(ctx.path, bytes, ctx.info.version);
      saveBtn.disabled    = false;
      saveBtn.textContent = 'Salvează';
      if (r.ok) {
        const v = p4Version(r);
        if (v) ctx.setVersion(v);
        ctx.setDirty(false);
        ctx.notify('Salvat.');
      } else {
        ctx.notify(explain(r), true);
      }
    };

    ctx.actions.append(saveBtn);
  }

  // ── PDF viewer ────────────────────────────────────────────────────────────

  function mountPdf(ctx) {
    const wrap   = document.createElement('div');
    wrap.className = 'le-pdf-wrap';
    const iframe = document.createElement('iframe');
    iframe.className = 'le-pdf-frame';
    // double-encode: outer enc for the ?file= param, inner enc for the download URL
    iframe.src = '/vendor/pdfjs/web/viewer.html?file=' +
      enc('/api/download?name=' + enc(ctx.path));
    wrap.append(iframe);
    ctx.body.append(wrap);
  }

  window.liveEdit.register(['csv', 'tsv'], mountCsv);
  window.liveEdit.register(['txt'],        mountTxt);
  window.liveEdit.register(['pdf'],        mountPdf);

  // ── Excel / Spreadsheet live editor (.xls, .xlsx, .ods) ──────────────────
  //
  // Uses SheetJS (loaded lazily from /vendor/sheetjs/xlsx.full.min.js).
  // Reuses .le-csv-* table classes so both grids look identical — only the
  // tab bar and outer wrapper use new .le-xls-* classes.

  async function mountExcel(ctx) {
    const { explain } = ctx.api;

    ctx.body.append(el('p', 'le-msg', 'Se încarcă…'));

    // -- 1. load SheetJS the first time it is needed --
    if (!window.XLSX) {
      try {
        await new Promise((resolve, reject) => {
          // don't double-inject if another tab triggered a load at the same time
          if (document.querySelector('script[src="/vendor/sheetjs/xlsx.full.min.js"]')) {
            // script tag exists but onload hasn't fired yet — poll
            const t = setInterval(() => { if (window.XLSX) { clearInterval(t); resolve(); } }, 40);
            return;
          }
          const s = document.createElement('script');
          s.src = '/vendor/sheetjs/xlsx.full.min.js';
          s.onload  = resolve;
          s.onerror = () => reject(new Error('SheetJS nu a putut fi încărcat de la /vendor/sheetjs/xlsx.full.min.js.'));
          document.head.append(s);
        });
      } catch (e) {
        ctx.body.textContent = '';
        ctx.body.append(el('p', 'le-msg bad', e.message));
        return;
      }
    }

    // -- 2. fetch raw bytes --
    let buf;
    try {
      const r = await fetch('/api/download?name=' + enc(ctx.path));
      if (!r.ok) {
        ctx.body.textContent = '';
        ctx.body.append(el('p', 'le-msg bad',
          'Eroare la descărcarea fișierului (' + r.status + ').'));
        return;
      }
      buf = await r.arrayBuffer();
    } catch {
      ctx.body.textContent = '';
      ctx.body.append(el('p', 'le-msg bad', 'Cannot reach the server.'));
      return;
    }

    // -- 3. parse workbook --
    let wb;
    try {
      wb = window.XLSX.read(buf, { type: 'array' });
    } catch (e) {
      ctx.body.textContent = '';
      ctx.body.append(el('p', 'le-msg bad', 'Fișierul nu a putut fi citit: ' + e.message));
      return;
    }

    ctx.body.textContent = ''; // clear loading spinner

    // grids[name] = 2D array; loaded lazily on first sheet access, mutated by editor
    const grids = {};
    function getGrid(name) {
      if (!grids[name]) {
        grids[name] = window.XLSX.utils.sheet_to_json(
          wb.Sheets[name], { header: 1, defval: '' }
        );
      }
      return grids[name];
    }

    let activeSheet = wb.SheetNames[0];

    // -- 4. build UI shell --
    const root = el('div', 'le-xls-root');

    // sheet tab bar — only shown when the workbook has more than one sheet
    let tabBar = null;
    if (wb.SheetNames.length > 1) {
      tabBar = el('div', 'le-xls-tabs');
      for (const name of wb.SheetNames) {
        const tab = el('button',
          'le-xls-tab' + (name === activeSheet ? ' on' : ''), name);
        tab.onclick = () => {
          activeSheet = name;
          tabBar.querySelectorAll('.le-xls-tab').forEach(t => {
            t.className = 'le-xls-tab' + (t.textContent === activeSheet ? ' on' : '');
          });
          renderGrid();
        };
        tabBar.append(tab);
      }
      root.append(tabBar);
    }

    const tableWrap = el('div', 'le-csv-wrap'); // reuse CSV scroll wrapper style
    const table     = el('table', 'le-csv-table');
    tableWrap.append(table);
    root.append(tableWrap);

    // -- helpers --
    function colCount(grid) {
      return grid.reduce((m, r) => Math.max(m, r.length), 0);
    }

    // 0 → 'A', 25 → 'Z', 26 → 'AA', …
    function colLabel(n) {
      let s = '';
      n++;
      while (n > 0) {
        s = String.fromCharCode(64 + (n % 26 || 26)) + s;
        n = Math.floor((n - 1) / 26);
      }
      return s;
    }

    // tap-to-edit cell — shares .le-csv-inp style with the CSV grid
    function makeCell(grid, ri, ci) {
      const td   = el('td', 'le-csv-td');
      const span = el('span', 'le-csv-cell-text');
      span.textContent = grid[ri][ci] != null ? String(grid[ri][ci]) : '';
      td.append(span);
      td.title = 'Apasă pentru a edita';
      td.onclick = () => {
        if (td.querySelector('input')) return;
        span.hidden = true;
        const inp = el('input', 'le-csv-inp');
        inp.value        = span.textContent;
        inp.autocomplete = 'off';
        td.append(inp);
        inp.focus(); inp.select();
        const commit = () => {
          const v = inp.value;
          while (grid[ri].length <= ci) grid[ri].push('');
          grid[ri][ci] = v;
          span.textContent = v;
          span.hidden = false;
          inp.remove();
          ctx.setDirty(true);
        };
        inp.addEventListener('blur', commit);
        inp.addEventListener('keydown', e => {
          if (e.key === 'Enter') { e.preventDefault(); commit(); }
          if (e.key === 'Tab')   { e.preventDefault(); commit(); }
          if (e.key === 'Escape') { span.hidden = false; inp.remove(); }
        });
      };
      return td;
    }

    function renderGrid() {
      table.textContent = '';
      const grid = getGrid(activeSheet);
      const cols = colCount(grid);

      // sticky header: A B C … column letters
      const thead = el('thead');
      const htr   = el('tr');
      htr.append(el('th', 'le-csv-th le-csv-rnum', '#'));
      for (let c = 0; c < cols; c++) htr.append(el('th', 'le-csv-th', colLabel(c)));
      htr.append(el('th', 'le-csv-th le-csv-del-hd', ''));
      thead.append(htr);
      table.append(thead);

      const tbody = el('tbody');
      for (let ri = 0; ri < grid.length; ri++) {
        const tr    = el('tr', 'le-csv-tr');
        const numTd = el('td', 'le-csv-td le-csv-rnum');
        numTd.textContent = ri + 1;
        tr.append(numTd);
        for (let ci = 0; ci < cols; ci++) tr.append(makeCell(grid, ri, ci));

        const delTd  = el('td', 'le-csv-td le-csv-del-td');
        const delBtn = el('button', 'le-csv-del-btn', '✕');
        delBtn.title = 'Șterge rândul';
        const row = ri; // capture loop variable
        delBtn.onclick = () => { grid.splice(row, 1); ctx.setDirty(true); renderGrid(); };
        delTd.append(delBtn);
        tr.append(delTd);
        tbody.append(tr);
      }
      table.append(tbody);
    }

    renderGrid();
    ctx.body.append(root);

    // -- 5. action buttons --
    const addRowBtn = el('button', 'btn le-csv-add-row', '+ Rând nou');
    addRowBtn.onclick = () => {
      const grid = getGrid(activeSheet);
      grid.push(Array(Math.max(colCount(grid), 1)).fill(''));
      ctx.setDirty(true);
      renderGrid();
    };

    const saveBtn = el('button', 'btn primary', 'Salvează');
    saveBtn.onclick = async () => {
      saveBtn.disabled    = true;
      saveBtn.textContent = 'Se salvează…';

      // flush every loaded (possibly edited) sheet back into the workbook object
      for (const name of Object.keys(grids)) {
        wb.Sheets[name] = window.XLSX.utils.aoa_to_sheet(grids[name]);
      }

      // pick the right export format
      const bookType = ctx.ext === 'xls' ? 'biff8'
                     : ctx.ext === 'ods' ? 'ods'
                     : 'xlsx';
      let out;
      try {
        out = window.XLSX.write(wb, { bookType, type: 'array' });
      } catch (e) {
        ctx.notify('Eroare la export: ' + e.message, true);
        saveBtn.disabled    = false;
        saveBtn.textContent = 'Salvează';
        return;
      }

      try {
        const r = await fetch(
          '/api/save?name=' + enc(ctx.path) + '&expected=' + enc(ctx.info.version),
          { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: out }
        );
        const text = await r.text();
        let json = null;
        try { json = JSON.parse(text); } catch {}

        if (r.ok) {
          if (json && json.version) ctx.setVersion(String(json.version));
          ctx.setDirty(false);
          ctx.notify('Salvat.');
        } else if (r.status === 409) {
          ctx.notify('Fișierul a fost modificat pe server. Reîncarcă fișierul.', true);
        } else {
          ctx.notify(
            (json && json.message) ? json.message
              : 'Eroare la salvare (' + r.status + ').',
            true
          );
        }
      } catch {
        ctx.notify('Cannot reach the server.', true);
      }

      saveBtn.disabled    = false;
      saveBtn.textContent = 'Salvează';
    };

    ctx.actions.append(addRowBtn, saveBtn);
  }

  window.liveEdit.register(['xls', 'xlsx', 'ods'], mountExcel);

})();
