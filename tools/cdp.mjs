// cdp.mjs: poke the kiosk's Chromium through the DevTools protocol (Node 22+).
// Chromium listens on the TV's 127.0.0.1:9222; tunnel it first, e.g.
//   ssh -N -L 9333:127.0.0.1:9222 root@TV &
// usage: node cdp.mjs <port> <cmd> [arg]   cmd: shot <file> | eval <expr> | key <Name>
//   shot: Chromium's own screenshot of the page (tools/tvshot shows the real screen)
const [port, cmd, arg] = process.argv.slice(2);
const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
await new Promise(r => ws.onopen = r);
const call = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const fs = await import('node:fs');
if (cmd === 'shot') {
  const r = await call('Page.captureScreenshot', { format: 'png' });
  if (r.error) console.log(JSON.stringify(r.error)); else { fs.writeFileSync(arg, Buffer.from(r.result.data, 'base64')); console.log('saved', arg); }
} else if (cmd === 'eval') {
  const r = await call('Runtime.evaluate', { expression: arg, returnByValue: true, awaitPromise: true });
  console.log(JSON.stringify(r.result?.result?.value ?? r));
} else if (cmd === 'key') {
  const map = { ArrowDown: 40, ArrowUp: 38, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27 };
  for (const type of ['rawKeyDown', 'keyUp']) await call('Input.dispatchKeyEvent', { type, key: arg, code: arg, windowsVirtualKeyCode: map[arg] });
  console.log('key', arg);
}
console.log('url', page.url);
ws.close(); process.exit(0);
