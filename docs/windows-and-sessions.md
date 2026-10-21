# Windows and sessions — implementation notes

Aura is a desktop browser for Linux and Windows. People need to keep separate
browsing tasks in separate windows while knowing which browsing is private.
The window command must open a real native window and preserve the existing
profile without restoring private addresses.

Design plan: reuse the current theme tokens (Aurora dark background #0D111B,
surface #182033, text #EDF3FF, muted #99A7C1, accent #A7B6FF), Inter regular and
medium, existing 12/13/15 px menu/toolbar type and 8 px spacing. Other themes
continue to use their own tokens. Put window commands in the Aura logo menu;
keep the existing persistent PRIVATE text and private-page explanation.

```
Aura logo / menu
  New window                 Ctrl N
  New private window   Ctrl Shift N
  Close window         Ctrl Shift W
  Exit Aura                  Ctrl Q
```

The same profile is shared inside one browser process. Each native window has
its own tabs, selection, dialogs and panels. The private context is temporary
and shared only by private tabs/windows; the session file contains only normal
windows. No extra decorative element is needed. Keyboard and pointer actions
must use the same commands; verify RU/EN and the smallest supported window.

Implementation and validation are recorded in `browser-roadmap-progress.md`.

Implemented behavior: Ctrl+N opens a normal native window, Ctrl+Shift+N opens a
private one, Ctrl+Shift+W closes that window and Ctrl+Q exits all windows. Existing
mixed private tabs remain available with Ctrl+Alt+N. Up to 32 windows can be open
at once. All windows share appearance preferences and the normal profile, while
each has independent selection, panels, dialogs and downloads.

The profile stores normal windows, selected tab and pinning. A private-only
remainder keeps the most recent normal session. A clean-exit marker distinguishes
an interrupted run. With automatic restoration disabled, interrupted startup asks
before restoring; the logo menu also offers the previous normal session. Restoring
an empty normal start window reuses it. Restoring from an occupied window opens
additional windows. This restores URLs, not submitted forms or in-page app state.

Closing the last private tab/window releases the shared temporary context. This
does not remove files explicitly downloaded to disk. Closed additional native
windows and their browser references are released before process shutdown.

Verification uses `--suite=windows` on the private CPU display: normal storage and
bookmarks sharing; private separation/sharing/reset; real close and exit; active
and pinned tab persistence; crash followed by UI recovery; no private URLs in
profile data; and balanced Chromium callbacks. Windows CI additionally exercises
normal/private native windows, shared language/scale and closing the root while
a child remains. Full storage/recovery scenarios run on Linux.

Launching Aura again with the same profile uses CEF's process singleton relay.
The running Aura process opens its own window for the requested URL/private mode;
the forwarding process exits normally. Extra theme, debugging and rendering
switches are not applied to the existing process through this relay.
