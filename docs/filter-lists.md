# Filter list management

Aura's built-in blocker serves people who need reliable browsing without managing
an extension. This panel must explain which lists are active, keep working rules
on update failures and let people add or correct their own filters.

Design plan: reuse theme tokens (Aurora dark #0D111B background, #182033 surface,
#EDF3FF text, #99A7C1 secondary text, #A7B6FF action), Inter 12/13 px controls and
23 px panel title, 8 px spacing. The primary action checks lists. Two list rows
show their state and date; custom rules have a separate editor and scrollable
list. Loading disables conflicting edits and exposes cancellation; failures keep
the entered rule. No additional decorative badge is needed.

```
Filter lists                              close
status                              Check / Cancel
EasyList           enabled       updated / bundled
EasyPrivacy        enabled       updated / bundled
parsed network / cosmetic / skipped
Custom rule [........................] Add / Save
[existing rules .....................] Remove
Import rules    Restore bundled lists
Limits and page reload guidance
```

List sources are the official EasyList repository, using HTTPS without cookies,
cache or redirects. Downloads are bounded and compiled outside the UI thread.
The active cache pointer is replaced only after complete validation and engine
compilation. Updates preserve custom rules and list choices. The bundled lists
remain available for recovery. Background checks can be disabled and never start
from a private-only browsing session.

Implementation and verification are tracked in `browser-roadmap-progress.md`.

## Rules and limits

Custom UTF-8 rules can be added, edited, removed or merged from a local regular
file. Comments and the first occurrence of each rule are preserved. Invalid or
unsupported custom rules leave the existing engine and entered text intact.
Limits: 1 MiB / 10,000 lines / 4,096 bytes per custom line; downloaded lists
8 MiB / 200,000 lines / 32,768 bytes per line, 60-second timeout per list.

Daily background checks begin after 30 seconds of normal browsing when enabled
in Protection settings. Failed attempts wait at least an hour before automatic
retry. Manual checks are explicit and cancellable. Only enabled sources download.
Generation directories store source/custom SHA-256 checksums and list choices.
The active pointer changes after validation and engine compilation succeed;
failed downloads/writes and cancellations keep the previous active engine.
A damaged startup cache falls back to bundled lists without deleting saved files.
Linux files are mode 0600 and directories 0700; Windows inherits profile ACLs.
Checksums detect local corruption, not authenticity against a modified profile.

The adapter uses the request's real HTTP method and supports the engine's
network exceptions and removal of query parameters from subresource requests.
Rewritten URLs must retain scheme, authority and path; top-level navigation is
not rewritten. Local resources are restricted to `noop.js`/`noopjs`,
`noop.css`/`noopcss`, and transparent `1x1.gif`/`1x1-transparent.gif`/
`transparent.gif`. Unknown redirect resources fall back to blocking. Replacements
do not contact the original server and do not disable CSP, SRI or CORS.

Structured cosmetic rules run in Aura's isolated world. Supported operations are
CSS selectors, literal `:has-text`, `:matches-path`, `:matches-attr`,
`:matches-css` (including before/after), `:min-text-length`, `:upward` (up to 32
ancestors) and `:style`. Processing is bounded to 256 procedural rules, 1,000
candidates per selection, 2,000 marked elements and an 8 ms budget between DOM
operations. Individual browser selector operations cannot be preempted.
Mutation changes cause reevaluation; styles/marks are removed when protection
is disabled. No filter text is executed with eval or as a regular expression.
Scriptlets, CSP response changes, regex/XPath procedural rules and destructive
DOM actions are unsupported. Remote unsupported rules are counted as skipped.
This is a supported subset rather than full uBlock Origin compatibility.

Latest published installers predate this feature. Build the current source to
use it until a new release is published.
