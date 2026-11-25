/* Tab changes only visibility; serial and capture sessions remain in place. */
(() => {
  const tabs = [...document.querySelectorAll('[role="tab"][data-page]')];
  function activate(page, focus = false) {
    if (page === 'configure') page = 'testing';
    if (!tabs.some(tab => tab.dataset.page === page)) page = 'device';
    for (const tab of tabs) {
      const active = tab.dataset.page === page;
      tab.setAttribute('aria-selected', String(active));
      tab.tabIndex = active ? 0 : -1;
      document.getElementById(tab.getAttribute('aria-controls')).hidden = !active;
      if (active && focus) tab.focus();
    }
    if (window.location.hash !== `#${page}`) window.history.replaceState(null, '', `#${page}`);
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => activate(tab.dataset.page));
    tab.addEventListener('keydown', event => {
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
      if (event.key === 'ArrowLeft') next = (index - 1 + tabs.length) % tabs.length;
      if (event.key === 'Home') next = 0;
      if (event.key === 'End') next = tabs.length - 1;
      if (next !== undefined) { event.preventDefault(); activate(tabs[next].dataset.page, true); }
    });
  });
  window.addEventListener('hashchange', () => activate(window.location.hash.slice(1)));
  activate(window.location.hash.slice(1));
})();
