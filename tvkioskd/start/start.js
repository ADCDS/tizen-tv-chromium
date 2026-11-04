// The kiosk browser's start page (served by tvkioskd): a search/address box
// and shortcuts to the configured apps. The tvkit extension provides the
// on-screen keyboard; Back here leaves the kiosk.
(async () => {
  const pt = (navigator.language || '').toLowerCase().startsWith('pt');
  const T = pt
    ? { title: 'Navegador', ph: 'Pesquisar ou digitar endereço', hint: 'OK no campo abre o teclado · Voltar volta para a TV' }
    : { title: 'Browser', ph: 'Search or type an address', hint: 'OK on the box opens the keyboard · Back returns to the TV' };
  document.documentElement.lang = pt ? 'pt-BR' : 'en';
  document.getElementById('title').textContent = T.title;
  document.getElementById('hint').textContent = T.hint;
  const q = document.getElementById('q');
  q.placeholder = T.ph;
  const clock = document.getElementById('clock');
  const tick = () => { clock.textContent = new Date().toLocaleTimeString(pt ? 'pt-BR' : 'en', { hour: '2-digit', minute: '2-digit' }); };
  tick(); setInterval(tick, 10000);

  let state = {};
  try { state = await (await fetch('/state')).json(); } catch (e) {}
  const colors = ['#d9534f', '#2d9cdb', '#27ae60', '#f39c12', '#8e44ad', '#16a085'];
  const box = document.getElementById('apps');
  const tiles = (state.apps || []).filter((a) => a.id !== 'browser').map((a, i) => {
    const t = document.createElement('div');
    t.className = 'app';
    const l = document.createElement('div');
    l.className = 'letter';
    l.style.background = colors[i % colors.length];
    l.textContent = (a.name || a.id).slice(0, 1).toUpperCase();
    const n = document.createElement('div');
    n.textContent = a.name || a.id;
    t.append(l, n);
    t.onclick = () => fetch('/open?app=' + encodeURIComponent(a.id)).catch(() => {});
    box.appendChild(t);
    return t;
  });

  // Focus: the box (index -1) or a tile.
  let sel = -1;
  const show = () => {
    q.classList.toggle('on', sel < 0);
    tiles.forEach((t, i) => t.classList.toggle('on', i === sel));
    if (sel < 0) q.focus(); else q.blur();
  };
  addEventListener('keydown', (e) => {
    if (e.defaultPrevented) return; // the keyboard (tvkit) has it
    const n = tiles.length;
    if (e.key === 'ArrowDown' && sel < 0 && n) sel = 0;
    else if (e.key === 'ArrowUp' && sel >= 0) sel = -1;
    else if (e.key === 'ArrowRight' && sel >= 0) sel = (sel + 1) % n;
    else if (e.key === 'ArrowLeft' && sel >= 0) sel = (sel - 1 + n) % n;
    else if (e.key === 'Enter' && sel >= 0) { tiles[sel].click(); return; }
    else return;
    e.preventDefault();
    show();
  });

  document.getElementById('f').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = q.value.trim();
    if (!text) return;
    let url;
    if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) url = text;
    else if (!/\s/.test(text) && /^(localhost|\d{1,3}(\.\d{1,3}){3}|[^/:]+\.local)(:\d+)?(\/.*)?$/i.test(text)) url = 'http://' + text;
    else if (!/\s/.test(text) && /^[^/\s]+\.[a-z]{2,}(:\d+)?(\/.*)?$/i.test(text)) url = 'https://' + text;
    else url = (state.search || 'https://www.google.com/search?q=%s').replace('%s', encodeURIComponent(text));
    location.href = url;
  });

  show();
  // Open the on-screen keyboard right away (tvkit listens for this).
  setTimeout(() => q.dispatchEvent(new CustomEvent('tvkit:keyboard', { bubbles: true })), 300);
})();
