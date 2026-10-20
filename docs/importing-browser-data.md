# Import bookmarks and history

Open **Settings → Data → Import browser data**. Use **Detected profiles** to choose
a standard Firefox, Chrome, Chromium or Brave profile, or choose/paste an export
file. Custom profile directories can also be pasted directly.

Supported sources:

| Source | What Aura reads |
|---|---|
| Firefox `places.sqlite` or its profile directory | Bookmarks and latest visits |
| Chromium profile directory | `Bookmarks` JSON and `History` SQLite together |
| Chrome/Chromium `Bookmarks` JSON | Bookmarks, including nested source folders |
| Chrome/Chromium `History` SQLite | Latest visit per web address |
| UTF-8 Netscape-style `.html` / `.htm` export | Bookmark links, names and dates |

Firefox compressed `.jsonlz4` backups are not supported; choose its HTML export or
`places.sqlite`. Close the source browser for a consistent snapshot, especially
when importing Chromium's two separate files. Committed SQLite WAL records are
supported, but locked, malformed or incompatible schemas produce an error.

Preview shows the source, item counts, skipped/duplicate records and sample
addresses. Check which data types to import, then click **Import selected data**.
Previewing or cancelling does not apply data to the Aura profile. The native file
chooser and path editor accept Unicode names and spaces.

Existing bookmarks keep their names. URLs are normalized before comparing them,
so host casing and default ports do not create new duplicates. Source bookmark
folders become Aura's flat bookmark list; folder organization is not preserved.
Only HTTP/HTTPS web addresses are imported; internal pages, bookmarklets and
local-file links are skipped. History is sorted and respects Aura's current size
and age limits. The result distinguishes added records from entries excluded by
those limits. Clear-on-exit settings still apply to imported history.

Before a change, Aura saves the current profile in a private
`PROFILE/import-backups/import-RANDOM/profile.toml` directory, then atomically
replaces its live profile file. A failed write leaves the in-memory profile as it
was. **Show backup folder** opens the saved copy. To restore it manually, close
Aura and copy that file over the profile's `profile.toml`. This restores the full
saved settings/bookmark/history/session snapshot, not cookies or downloaded files.
Keep a copy of the current file before doing a manual restoration.

Source databases are opened with SQLite's `SQLITE_OPEN_READONLY`; JSON and HTML
are parsed as data without executing scripts or loading their web addresses.
Profile discovery only checks standard directories when requested; it does not
read unselected histories, saved passwords or cookies. The importer reads at most
100,000 records of each type, limits decoded record data to 64 MB, and marks
partial previews. Text exports are capped at 64 MB; database files at 2 GB.
Long-running reads can be cancelled. SQLite uses defensive configuration,
an untrusted schema, bounded caches and a progress deadline.

Format references: [Chromium bookmark codec](https://chromium.googlesource.com/chromium/chromium/+/HEAD/components/bookmarks/browser/bookmark_codec.cc),
[Mozilla Places schema](https://github.com/mozilla/application-services/blob/main/components/places/src/db/schema.rs),
[SQLite read-only connections](https://www.sqlite.org/c3ref/open.html),
[SQLite defensive configuration](https://www.sqlite.org/c3ref/c_dbconfig_defensive.html).

## Verification

`aura_import_tests` uses generated HTML/JSON and Firefox/Chromium SQLite fixtures,
including committed WAL, Unicode filenames, duplicates, retention, cancellation,
write failure and a 100,001-record history. `verify_browser.py --suite=import`
uses fake profiles and an isolated CPU display to test the actual UI, native
chooser, selection, backups, language/scale changes and persistence. It does not
import the machine owner's real browser data.
