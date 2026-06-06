// Minimal lightbox for the front-page feature screenshots: clicking an
// `a.feature-lightbox` link opens the full-size image in an in-page
// overlay instead of navigating to the file. Close by clicking anywhere
// or pressing Escape. Self-hosted, no dependencies.
document.addEventListener('click', function (e) {
  var link = e.target.closest('a.feature-lightbox');
  if (!link || e.metaKey || e.ctrlKey || e.shiftKey || e.button !== 0) return;
  e.preventDefault();

  var overlay = document.createElement('div');
  overlay.className = 'lightbox-overlay';
  var img = document.createElement('img');
  img.src = link.href;
  var thumb = link.querySelector('img');
  img.alt = thumb ? thumb.alt : '';
  overlay.appendChild(img);

  function close() {
    overlay.remove();
    document.removeEventListener('keydown', onKey);
  }
  function onKey(ev) {
    if (ev.key === 'Escape') close();
  }
  overlay.addEventListener('click', close);
  document.addEventListener('keydown', onKey);
  document.body.appendChild(overlay);
});
