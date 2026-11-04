// tvkit (page world): keep everything in the current tab. The kiosk has no tab
// strip, so a page opened in a new tab or window would be stranded.
(() => {
  if (window.top !== window) return;
  window.open = function (url) {
    if (url) location.href = new URL(String(url), location.href).href;
    return null;
  };
})();
