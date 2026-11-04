// tvkit service worker: talks to the TV helper (tvkioskd) for the pages, which
// can't reach http://127.0.0.1 themselves (mixed content, private network).
const HELPER = 'http://127.0.0.1:7770';

chrome.runtime.onMessage.addListener((msg, sender, reply) => {
  if (msg.t === 'leave') {
    fetch(HELPER + '/leave', { method: 'POST' }).catch(() => {}).finally(() => reply({}));
  } else if (msg.t === 'state') {
    fetch(HELPER + '/state').then((r) => r.json()).then(reply, () => reply(null));
  } else {
    return false;
  }
  return true; // reply comes later
});
