// tvkit: remote-control helpers for the TV kiosk (content script, top frame).
//  - On-screen keyboard: OK on a text field opens it. Arrows move, OK types,
//    Back closes. Text goes in through execCommand('insertText'), so pages
//    (React included) see ordinary input events. Remote digits type directly.
//  - Red A (F13, remapped by wlxbridge) opens a bar: Back, Forward, Reload,
//    Home, Address, Exit.
//  - Back (Escape): on the configured apps (TV-ready pages like Novevo) the page
//    gets it first, and only if it doesn't handle it do we go back or leave. On
//    other sites (general browsing) Back always goes back: out of fullscreen, to
//    the previous page, or with no history left, out of the kiosk (back to the TV).
//  - Pointer mode (wlxbridge, green B) moves the real mouse; the cursor isn't
//    part of the GPU output, so it is drawn here.
(() => {
  if (window.top !== window || window.__tvkit) return;
  window.__tvkit = true;

  const PT = (navigator.language || '').toLowerCase().startsWith('pt');
  const T = PT
    ? { back: 'Voltar', fwd: 'Avançar', reload: 'Recarregar', home: 'Início', addr: 'Endereço', exit: 'Sair', done: 'OK', clear: 'Limpar', space: 'espaço', hint: 'Setas e OK para digitar · Voltar fecha' }
    : { back: 'Back', fwd: 'Forward', reload: 'Reload', home: 'Home', addr: 'Address', exit: 'Exit', done: 'OK', clear: 'Clear', space: 'space', hint: 'Arrows and OK to type · Back closes' };

  const send = (msg) => new Promise((res) => {
    try { chrome.runtime.sendMessage(msg, (r) => { void chrome.runtime.lastError; res(r); }); } catch (e) { res(null); }
  });
  const leave = () => send({ t: 'leave' });

  // ---- overlay root (closed shadow DOM, above everything) ----
  let root = null;
  function overlay() {
    if (root) return root;
    const host = document.createElement('tvkit-overlay');
    host.style.cssText = 'all:initial;position:fixed;inset:0;z-index:2147483647;pointer-events:none';
    root = host.attachShadow({ mode: 'closed' });
    const st = document.createElement('style');
    st.textContent = CSS;
    root.appendChild(st);
    (document.documentElement || document).appendChild(host);
    return root;
  }
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  const CSS = `
    * { box-sizing: border-box; font-family: "DejaVu Sans", "Liberation Sans", sans-serif; }
    .osk { position: fixed; left: 50%; bottom: 10px; transform: translateX(-50%); width: 760px;
      background: rgba(18,20,26,.96); border-radius: 12px; padding: 8px 10px 8px; color: #eee;
      box-shadow: 0 6px 30px rgba(0,0,0,.6); }
    .preview { background: #0b0c10; border: 2px solid #2d6cdf; border-radius: 8px; padding: 6px 10px;
      font-size: 17px; min-height: 32px; white-space: pre; overflow: hidden; text-overflow: ellipsis; margin-bottom: 4px; }
    .preview .caret { display: inline-block; width: 2px; height: 19px; background: #8ab4ff; vertical-align: middle; }
    .row { display: flex; gap: 5px; margin-top: 5px; justify-content: center; }
    .key { flex: 0 0 auto; width: 68px; height: 36px; border-radius: 7px; background: #2a2e38; color: #f2f2f2;
      display: flex; align-items: center; justify-content: center; font-size: 17px; border: 2px solid transparent; }
    .key.wide { width: 212px; } .key.mid { width: 102px; }
    .key.on { background: #2d6cdf; border-color: #fff; transform: scale(1.06); }
    .key.toggled { background: #4a5163; }
    .hint { font-size: 11px; color: #9aa; text-align: center; margin-top: 5px; }
    .bar { position: fixed; top: 10px; left: 50%; transform: translateX(-50%); display: flex; gap: 8px;
      background: rgba(18,20,26,.97); padding: 8px; border-radius: 12px; box-shadow: 0 6px 30px rgba(0,0,0,.6); max-width: 940px; }
    .btn { height: 44px; padding: 0 14px; border-radius: 8px; background: #2a2e38; color: #f2f2f2; font-size: 17px;
      display: flex; align-items: center; border: 2px solid transparent; white-space: nowrap; }
    .btn.url { max-width: 380px; overflow: hidden; text-overflow: ellipsis; color: #bcd; }
    .btn.on { background: #2d6cdf; border-color: #fff; color: #fff; }
    .cursor { position: fixed; width: 24px; height: 32px; margin: -2px 0 0 -2px; display: none;
      filter: drop-shadow(1px 2px 2px rgba(0,0,0,.6)); }
  `;

  // ---- on-screen keyboard ----
  const LAYOUTS = {
    abc: ['1234567890', 'qwertyuiop', 'asdfghjklç', 'zxcvbnm,.-'],
    sym: ['áàâãéêíóôõ', 'úüñ@#$%&*+', '/:_?!"\'()=', '.com;~^[]<>'],
  };
  const osk = {
    open: false, target: null, layout: 'abc', shift: false, sel: [1, 0], keys: [], box: null, preview: null,
    // target: an editable element, or {value, done(text)} for a virtual field (address)
    show(target) {
      this.close();
      this.target = target;
      this.layout = 'abc'; this.shift = false; this.sel = [1, 0];
      this.box = el('div', 'osk');
      this.preview = el('div', 'preview');
      this.box.appendChild(this.preview);
      this.rowsEl = el('div');
      this.box.appendChild(this.rowsEl);
      this.box.appendChild(el('div', 'hint', T.hint));
      overlay().appendChild(this.box);
      this.open = true;
      this.render();
    },
    close() {
      if (this.box) this.box.remove();
      this.box = null; this.open = false;
    },
    value() {
      const t = this.target;
      if (!t) return '';
      if (t.done) return t.value;
      if (t.isContentEditable) return t.innerText;
      return t.type === 'password' ? '•'.repeat(t.value.length) : t.value;
    },
    render() {
      this.rowsEl.textContent = '';
      this.keys = [];
      const rows = LAYOUTS[this.layout].map((r) => (r === '.com;~^[]<>' ? ['.com', ...';~^[]<>'] : [...r]));
      rows.forEach((r) => this.keys.push(r.map((ch) => ({ label: this.shift ? ch.toUpperCase() : ch, act: 'char' }))));
      this.keys.push([
        { label: '⇧', act: 'shift', cls: 'mid', toggled: this.shift },
        { label: this.layout === 'abc' ? 'áé@' : 'abc', act: 'layout', cls: 'mid' },
        { label: T.space, act: 'space', cls: 'wide' },
        { label: '⌫', act: 'bksp', cls: 'mid' },
        { label: T.clear, act: 'clear', cls: 'mid' },
        { label: T.done, act: 'done', cls: 'mid' },
      ]);
      this.keys.forEach((row, ri) => {
        const r = el('div', 'row');
        row.forEach((k, ci) => {
          k.el = el('div', 'key' + (k.cls ? ' ' + k.cls : '') + (k.toggled ? ' toggled' : ''), k.label);
          if (ri === this.sel[0] && ci === this.sel[1]) k.el.classList.add('on');
          r.appendChild(k.el);
        });
        this.rowsEl.appendChild(r);
      });
      this.updatePreview();
    },
    updatePreview() {
      this.preview.textContent = this.value();
      this.preview.appendChild(el('span', 'caret'));
    },
    move(dr, dc) {
      const [r, c] = this.sel;
      if (dc) {
        const n = this.keys[r].length;
        this.sel = [r, (c + dc + n) % n];
      } else {
        const nr = (r + dr + this.keys.length) % this.keys.length;
        const x = this.keys[r][c].el.getBoundingClientRect();
        const cx = x.left + x.width / 2;
        let best = 0, bd = 1e9;
        this.keys[nr].forEach((k, i) => {
          const b = k.el.getBoundingClientRect();
          const d = Math.abs(b.left + b.width / 2 - cx);
          if (d < bd) bd = d, best = i;
        });
        this.sel = [nr, best];
      }
      this.keys.flat().forEach((k) => k.el.classList.remove('on'));
      this.keys[this.sel[0]][this.sel[1]].el.classList.add('on');
    },
    insert(text) {
      const t = this.target;
      if (t.done) t.value += text;
      else { t.focus(); document.execCommand('insertText', false, text); }
      this.updatePreview();
    },
    press() {
      const k = this.keys[this.sel[0]][this.sel[1]];
      const t = this.target;
      switch (k.act) {
        case 'char': this.insert(k.label); if (this.shift) { this.shift = false; this.render(); } break;
        case 'space': this.insert(' '); break;
        case 'shift': this.shift = !this.shift; this.render(); break;
        case 'layout': this.layout = this.layout === 'abc' ? 'sym' : 'abc'; this.render(); break;
        case 'bksp':
          if (t.done) t.value = t.value.slice(0, -1);
          else { t.focus(); document.execCommand('delete'); }
          this.updatePreview(); break;
        case 'clear':
          if (t.done) t.value = '';
          else { t.focus(); document.execCommand('selectAll'); document.execCommand('delete'); }
          this.updatePreview(); break;
        case 'done': this.close(); submit(t); break;
      }
    },
    key(e) {
      switch (e.key) {
        case 'ArrowUp': this.move(-1, 0); break;
        case 'ArrowDown': this.move(1, 0); break;
        case 'ArrowLeft': this.move(0, -1); break;
        case 'ArrowRight': this.move(0, 1); break;
        case 'Enter': this.press(); break;
        case 'Escape': case 'F13': this.close(); break;
        case 'PageUp': case 'PageDown': break;
        default:
          if (/^[0-9]$/.test(e.key)) this.insert(e.key);
      }
    },
  };

  // Done: hand the text to the page as if Enter was pressed in the field.
  let synthetic = false;
  function submit(t) {
    if (t.done) return t.done(t.value);
    t.focus();
    synthetic = true;
    const init = { key: 'Enter', code: 'Enter', bubbles: true, cancelable: true };
    const go = t.dispatchEvent(new KeyboardEvent('keydown', init));
    t.dispatchEvent(new KeyboardEvent('keypress', init));
    t.dispatchEvent(new KeyboardEvent('keyup', init));
    synthetic = false;
    if (go && t.form) t.form.requestSubmit ? t.form.requestSubmit() : t.form.submit();
  }

  function isEditable(e) {
    if (!e) return false;
    if (e.isContentEditable) return true;
    if (e.tagName === 'TEXTAREA') return !e.readOnly && !e.disabled;
    if (e.tagName !== 'INPUT' || e.readOnly || e.disabled) return false;
    return ['text', 'search', 'url', 'email', 'password', 'tel', 'number', ''].includes(e.type);
  }

  // ---- addresses and searches ----
  async function go(text) {
    text = text.trim();
    if (!text) return;
    let url;
    if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) url = text;
    else if (!/\s/.test(text) && /^(localhost|\d{1,3}(\.\d{1,3}){3}|[^/:]+\.local)(:\d+)?(\/.*)?$/i.test(text)) url = 'http://' + text;
    else if (!/\s/.test(text) && /^[^/\s]+\.[a-z]{2,}(:\d+)?(\/.*)?$/i.test(text)) url = 'https://' + text;
    else {
      const st = await send({ t: 'state' });
      const s = (st && st.search) || 'https://www.google.com/search?q=%s';
      url = s.replace('%s', encodeURIComponent(text));
    }
    location.href = url;
  }

  async function home() {
    const st = await send({ t: 'state' });
    const apps = (st && st.apps) || [];
    const mine = apps.find((a) => { try { return new URL(a.url).origin === location.origin; } catch (e) { return false; } });
    const browser = apps.find((a) => a.id === 'browser');
    location.href = (mine || browser || { url: 'http://127.0.0.1:7770/start' }).url;
  }

  // ---- browser bar ----
  const bar = {
    open: false, box: null, items: [], sel: 0,
    show() {
      this.close();
      const actions = [
        [T.back, () => history.back()],
        [T.fwd, () => history.forward()],
        [T.reload, () => location.reload()],
        [T.home, home],
        [location.host + location.pathname, () => osk.show({ value: location.href, done: go }), 'url'],
        [T.exit, leave],
      ];
      this.box = el('div', 'bar');
      this.items = actions.map(([label, fn, cls]) => {
        const b = el('div', 'btn' + (cls ? ' ' + cls : ''), label);
        this.box.appendChild(b);
        return { b, fn };
      });
      this.sel = 0;
      this.items[0].b.classList.add('on');
      overlay().appendChild(this.box);
      this.open = true;
    },
    close() { if (this.box) this.box.remove(); this.box = null; this.open = false; },
    key(e) {
      const n = this.items.length;
      if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        this.items[this.sel].b.classList.remove('on');
        this.sel = (this.sel + (e.key === 'ArrowLeft' ? -1 : 1) + n) % n;
        this.items[this.sel].b.classList.add('on');
      } else if (e.key === 'Enter') {
        const fn = this.items[this.sel].fn;
        this.close();
        fn();
      } else if (e.key === 'Escape' || e.key === 'F13') this.close();
    },
  };

  // ---- Back ----
  // Is this page one of the configured apps (which handle Back themselves)?
  let appPage = false;
  send({ t: 'state' }).then((st) => {
    appPage = !!(st && st.apps || []).find((a) => {
      try { return a.id !== 'browser' && new URL(a.url).origin === location.origin; } catch (e) { return false; }
    });
  });
  function backNow() {
    if (document.fullscreenElement) document.exitFullscreen();
    else if (window.navigation && navigation.canGoBack) history.back();
    else leave();
  }
  // Apps: Back that the page doesn't handle. A page may also swallow Back
  // without doing anything visible (Novevo on some screens): a second Back on
  // the same page, with nothing changed, then goes back or leaves anyway.
  let lastBack = { href: '', t: 0 };
  function backLater(e) {
    const href = location.href;
    const fullscreen = !!document.fullscreenElement;
    const again = lastBack.href === href && Date.now() - lastBack.t < 8000;
    lastBack = { href, t: Date.now() };
    setTimeout(() => {
      if (fullscreen || location.href !== href) return; // the page (or the browser) handled it
      if (e.defaultPrevented && !again) return; // e.g. the page closed a dialog
      lastBack = { href: '', t: 0 };
      if (window.navigation && navigation.canGoBack) history.back();
      else leave();
    }, 250);
  }

  // ---- key routing (capture phase: before the page) ----
  const swallowed = new Set();
  function swallow(e) {
    e.preventDefault();
    e.stopImmediatePropagation();
  }
  window.addEventListener('keydown', (e) => {
    if (synthetic || !e.isTrusted) return;
    let mine = false;
    if (osk.open) { osk.key(e); mine = true; }
    else if (bar.open) { bar.key(e); mine = true; }
    else if (e.key === 'F13') { bar.show(); mine = true; }
    else if (e.key === 'Enter' && isEditable(document.activeElement)) { osk.show(document.activeElement); mine = true; }
    else if (e.key === 'Escape' && appPage) backLater(e);
    else if (e.key === 'Escape') { backNow(); mine = true; }
    if (mine) { swallowed.add(e.code); swallow(e); }
  }, true);
  for (const type of ['keypress', 'keyup']) {
    window.addEventListener(type, (e) => {
      if (synthetic || !swallowed.has(e.code)) return;
      if (type === 'keyup') swallowed.delete(e.code);
      swallow(e);
    }, true);
  }
  // Pages can ask for the keyboard on a field (the start page does on load).
  document.addEventListener('tvkit:keyboard', (e) => { if (isEditable(e.target)) osk.show(e.target); }, true);

  // ---- links that would open a new tab stay in this one ----
  document.addEventListener('click', (e) => {
    const a = e.target && e.target.closest && e.target.closest('a[target]');
    if (a && a.target !== '_self' && a.target !== '_top' && a.target !== '_parent') a.target = '_self';
  }, true);

  // ---- pointer cursor ----
  let cursor = null, hideTimer = 0;
  window.addEventListener('mousemove', (e) => {
    if (!e.isTrusted) return;
    if (!cursor) {
      cursor = el('div', 'cursor');
      cursor.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="32" viewBox="0 0 24 32">' +
        '<path d="M2 2 L2 26 L8 20 L12 30 L16 28 L12 18 L20 18 Z" fill="#fff" stroke="#000" stroke-width="2" stroke-linejoin="round"/></svg>';
      overlay().appendChild(cursor);
    }
    const parked = e.clientX >= innerWidth - 2 && e.clientY >= innerHeight - 2; // wlxbridge parks it there
    cursor.style.display = parked ? 'none' : 'block';
    cursor.style.left = e.clientX + 'px';
    cursor.style.top = e.clientY + 'px';
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => { cursor.style.display = 'none'; }, 5000);
  }, true);
})();
