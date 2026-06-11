// Collapsible navigation sidebar for the sphinx_rtd_theme docs.
//
// The theme keeps the left navigation always open on desktop (only a
// mobile hamburger collapses it).  This adds a small floating button
// that hides the sidebar and lets the page text use the full width,
// remembering the choice across pages; the backslash key toggles it too.
(function () {
  var KEY = 'ps-sidebar-collapsed';
  var btn = null;

  function isOn() {
    return document.body.classList.contains('ps-sidebar-collapsed');
  }

  function render() {
    var on = isOn();
    if (btn) {
      btn.innerHTML = on ? '»' : '«'; // » when collapsed, « when open
      btn.setAttribute('aria-pressed', on ? 'true' : 'false');
    }
  }

  function set(on) {
    document.body.classList.toggle('ps-sidebar-collapsed', on);
    try {
      localStorage.setItem(KEY, on ? '1' : '0');
    } catch (e) {
      /* private mode / storage disabled: in-session only */
    }
    render();
  }

  function init() {
    btn = document.createElement('button');
    btn.id = 'ps-sidebar-toggle';
    btn.type = 'button';
    btn.title = 'Toggle navigation sidebar (\\)';
    btn.setAttribute('aria-label', 'Toggle navigation sidebar');
    document.body.appendChild(btn);

    var saved = false;
    try {
      saved = localStorage.getItem(KEY) === '1';
    } catch (e) {
      /* ignore */
    }
    document.body.classList.toggle('ps-sidebar-collapsed', saved);
    render();

    btn.addEventListener('click', function () {
      set(!isOn());
    });
  }

  document.addEventListener('keydown', function (e) {
    if (e.key !== '\\' || e.ctrlKey || e.metaKey || e.altKey) {
      return;
    }
    var tag = (e.target && e.target.tagName) || '';
    if (/^(INPUT|TEXTAREA|SELECT)$/.test(tag) || (e.target && e.target.isContentEditable)) {
      return;
    }
    e.preventDefault();
    set(!isOn());
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
