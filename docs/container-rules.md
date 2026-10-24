# Правила контейнеров / Container rules

Новая функция в исходниках после v0.4.0-preview.2; публичные установщики пока
не обновлены.

Откройте «Контейнеры сайтов» (Ctrl+Alt+C) → «Правила сайтов». Введите домен,
например `example.org`, выберите контейнер в списке и нажмите «Сохранить».
Опция «Включая поддомены» распространяет правило на `docs.example.org`.
Более точное правило имеет приоритет. Схема HTTP/HTTPS, порт и путь правило
не меняют. Международные имена нормализует Chromium.

При переходе в другой контейнер Aura сначала спрашивает подтверждение.
«Открыть вкладку» открывает адрес в новой вкладке с отдельными cookies;
исходная вкладка остаётся открытой. «Отмена» останавливает переход.
В том же контейнере подтверждение не требуется. Правила относятся к верхней
странице, включая редиректы, а не к изображениям, iframe и другим ресурсам сайта.

Приватные вкладки игнорируют эти правила и сохраняют приватный контекст.
Отправка POST и других запросов с методом, отличным от GET, в другой контейнер
блокируется с подсказкой: сначала откройте сайт в назначенном контейнере и
заполните форму там. Aura не пересылает введённые данные и не заменяет POST на GET.

Правило можно изменить или удалить в этом же списке. Сохранение того же домена
обновляет существующее правило. До 256 правил на профиль; они действуют во всех
его окнах и переживают перезапуск. Удаление контейнера удаляет его правила.

---

Open **Site containers** (Ctrl+Alt+C) → **Site rules**. Enter a hostname, select a
container from the menu, and choose **Save rule**. **Include subdomains** also
matches hosts such as `docs.example.org`; a more specific rule takes precedence.
HTTP/HTTPS, ports and paths share the same rule. Chromium normalizes international
hostnames.

A navigation into another container pauses for confirmation. **Open tab** opens
the address in a new tab with that container's cookies and leaves the original tab
open. **Cancel** stops the navigation. Matching requests within the assigned
container proceed normally. Rules apply to top-level pages and redirects, not to
iframes, images or other subresources.

Private tabs ignore these rules. Cross-container POST and other non-GET methods
are blocked with an explanation: open the site in its assigned container before
filling in the form. Aura never resubmits form data or changes POST into GET.

Use **Edit** or **Remove** in the rule list. Saving the same hostname updates its
rule. Up to 256 rules persist per profile and are shared by all its windows.
Removing a container also removes its rules.
