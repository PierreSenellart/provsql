// Compatibility shim: sphinx-copybutton 0.4.0 reads
// DOCUMENTATION_OPTIONS.URL_ROOT, which Sphinx >= 5 has removed in favour of
// the `<html data-content_root="...">` attribute.  Without this shim the
// copy-button image src ends up as `undefined_static/copy-button.svg`.
// Drop this file once sphinx-copybutton >= 0.5.2 is in the toolchain.
(function () {
  if (typeof DOCUMENTATION_OPTIONS !== 'undefined' &&
      DOCUMENTATION_OPTIONS.URL_ROOT === undefined) {
    DOCUMENTATION_OPTIONS.URL_ROOT =
        document.documentElement.dataset.content_root || '';
  }
})();
