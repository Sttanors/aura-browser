# Доступ расширений к сайтам / Extension website access

## Русский

Откройте **Расширения → Доступ к сайтам** у установленного расширения.
С клавиатуры: **Ctrl+K → Управление расширениями**, затем F10/Tab и Enter.
Управление доступно из обычной вкладки основного контейнера. Для нового или
обновлённого расширения сначала выполните предложенный перезапуск Aura.

- **Не запускать автоматически** отзывает автоматический доступ к содержимому
  сайтов и соответствующие разрешения host API. Это не выключение расширения:
  его popup, хранилище и другие разрешённые API могут продолжать работать.
- **На выбранных сайтах** разрешает доступ к добавленным адресам. Введите,
  например, `https://example.com`, нажмите **Добавить сайт** или Enter.
  Протокол и точный домен учитываются; порты не различаются. `example.com`
  не включает `www.example.com` или другие поддомены. Используются адреса ASCII
  либо punycode. Ошибочный адрес и дубликат остаются в поле для исправления.
- **На всех запрошенных сайтах** возвращает доступ в пределах разрешений,
  запрошенных расширением. Это не выдача доступа к произвольным новым сайтам.

**Удалить** отзывает выбранное разрешение. После ограничения доступа перезагрузите
открытые страницы; соответствующая кнопка есть в панели. Уже выполненный код и
изменения документа нельзя отменить одним изменением разрешений. Перезагрузка
может очистить несохранённые формы, поэтому Aura оставляет её пользователю.

Настройки сохраняет Chromium в профиле; они действуют во всех обычных окнах того
же профиля. **Обновить** повторно читает фактические разрешения движка, в том числе
после изменения настроек в другом окне. При удалении последнего разрешённого
сайта Chromium может представлять пустой список как отсутствие автоматического
доступа. Выберите режим выбранных сайтов, чтобы добавить новый адрес.

Нельзя добавить сайт, которого расширение не запрашивало. У расширения без
настраиваемых host-разрешений показывается объяснение вместо активных переключателей.
Другие разрешения, например правила `declarativeNetRequest`, подчиняются собственным
правилам Chromium; эта панель не обещает полностью отключить любые сетевые действия.
Чтобы полностью выключить расширение, используйте **Отключить** и перезапуск Aura.

**Ограничение CEF:** временный `activeTab` по нажатию на собственные кнопки Aura
пока не реализован. Поэтому первый режим честно называется «Не запускать
автоматически», а не «По нажатию». Приватные вкладки и отдельные контейнеры не
получают расширения через эту панель.

## English

Open **Extensions → Site access** beside an installed extension. For keyboard
access, use **Ctrl+K → Manage extensions**, then F10/Tab and Enter. Use a regular
tab in the default container; finish the requested restart after installation
or an extension update first.

- **Block automatic access** withholds automatic page access and the related
  host API permissions. It does not disable the extension: its popup, storage
  and other permitted APIs may remain active.
- **On selected sites** enables access to added addresses. Enter an address such
  as `https://example.com`, then select **Add site** or press Enter. Scheme and
  exact hostname matter; ports do not. Subdomains require separate entries.
  ASCII/punycode addresses are supported. Invalid or duplicate drafts remain
  available for correction.
- **On all requested sites** restores access within the extension's requested
  permissions. It does not grant arbitrary new website permissions.

**Remove** revokes a grant. Reload open pages after restricting access; the panel
includes a reload button. Permission changes cannot undo code or document changes
that already ran. Reload may clear unsaved forms, so Aura leaves it to the user.

Chromium persists the settings for ordinary windows of the same profile. **Refresh**
reads the engine's current state, including changes made from another window.
Chromium may report an empty selected-site list as withheld automatic access;
select the specific-sites option again to enter a new address.

Sites outside the extension's requested hosts are rejected. Extensions without
adjustable host permissions display an explanation. Separate permissions such as
`declarativeNetRequest` retain their own Chromium semantics; this panel does not
promise to disable every network action. Use **Disable** and restart Aura to turn
an extension off completely.

**CEF limitation:** Aura does not yet invoke temporary `activeTab` permissions
through its own toolbar buttons. The first option is therefore labelled **Block
automatic access**, not **On click**. This panel does not activate extensions in
private tabs or separate containers.

## Implementation and validation

The native Skia panel reads and updates Chromium runtime host permissions through
`developerPrivate` in a dedicated, initially hidden Chrome-style Views window.
CEF Alloy/OSR blocks `chrome://extensions`, so the helper uses Chrome style while
the user's interface stays in Aura. Its main-frame navigation is restricted to
that exact trusted WebUI URL; no renderer bridge is exposed to visited websites.
No remote debugging port or unsafe-extension-debugging flag is enabled.

Only one operation runs at a time, with a timeout and engine readback after
mutation errors. The engine's matching-extension query checks requested hosts
before granting a site. User strings are JSON-escaped; credentials, wildcard
schemes/hosts, paths and executable schemes are rejected by the native editor.
The helper closes after a renderer failure/timeout and can be recreated. Exit
waits for both its CEF browser and native window; global lifecycle counts include
it. The extension registry, signed packages and installed manifests are unchanged.

Primary sources: [CEF Alloy limitation](https://github.com/chromiumembedded/cef/issues/3859),
[Chromium developerPrivate implementation](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/extensions/api/developer_private/developer_private_functions.cc),
[API schema](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/common/extensions/api/developer_private.webidl),
[Chrome activeTab semantics](https://developer.chrome.com/docs/extensions/develop/concepts/activeTab).
