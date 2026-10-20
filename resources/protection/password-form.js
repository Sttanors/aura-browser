// Evaluated only after an explicit browser UI action, in an isolated main-frame world.
((mode, expectedURL, login) => {
  if (window !== window.top || location.protocol !== 'https:' || location.href !== expectedURL)
    return {error: 'The page changed. Try again.'};
  const visible = element => !element.disabled && !element.readOnly && element.getClientRects().length &&
    getComputedStyle(element).visibility === 'visible' && getComputedStyle(element).display !== 'none';
  const passwords = [...document.querySelectorAll('input[type=password]')].filter(input =>
    visible(input) && input.autocomplete !== 'new-password' && input.autocomplete !== 'one-time-code');
  if (passwords.length !== 1) return {error: 'Open a page with one visible login form.'};
  const password = passwords[0], form = password.form;
  if (!form || new URL(form.action || location.href, location.href).origin !== location.origin)
    return {error: 'The login form must submit to this HTTPS site.'};
  const users = [...form.querySelectorAll('input')].filter(input =>
    ['text', 'email'].includes(input.type) && visible(input) &&
    input.autocomplete !== 'one-time-code' &&
    (input.compareDocumentPosition(password) & Node.DOCUMENT_POSITION_FOLLOWING));
  const user = users.find(input => input.autocomplete === 'username') || users.at(-1);
  if (mode === 'read') {
    if (!password.value || password.value.length > 8192 || (user?.value.length || 0) > 4096)
      return {error: 'Enter a username and password in the page first.'};
    return {origin: location.origin, username: user?.value || '', password: password.value};
  }
  if (mode !== 'fill' || !login || login.origin !== location.origin)
    return {error: 'This login belongs to a different HTTPS site.'};
  const set = (element, value) => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(element, value);
    element.dispatchEvent(new Event('input', {bubbles: true}));
    element.dispatchEvent(new Event('change', {bubbles: true}));
  };
  if (user) set(user, login.username);
  set(password, login.password);
  return {filled: true}; // Never submit the form.
})
