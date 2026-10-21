# Extension lifecycle

These changes are in source builds after `v0.2.0-preview.3.1`; installers from
that release do not include them.

Open the puzzle menu to install a signed Manifest V3 CRX, pin an extension,
open its popup/options, disable it, remove it or check for an update.
For a Chrome Web Store installation, **Check** downloads the same ID.
For a local installation, select a newer signed CRX from its publisher.
Checks are explicit, not periodic background updates.

Before installation Aura verifies the CRX3 signature and ID, validates the
manifest/version and shows permissions. Updates highlight added permissions;
optional permissions are labelled separately. A permission listed here is a
request by the extension, not proof that the engine implements the API.
Older versions are rejected; equal versions are reported as already installed.
An update preserves pinning and the enabled/disabled choice. Downloading can
be cancelled and has a 45-second timeout; package reads and verification run
outside the UI thread. Limits are 50 MB compressed, 150 MB extracted and 15,000
archive entries. Path traversal and links are rejected.

## Applying changes

Install, update, enable, disable and removal require a restart. Use **Restart
Aura** in the manager and confirm. Aura saves normal tabs, closes private tabs,
shuts Chromium down and then starts the same executable with the same profile.
This one-time restoration does not change the startup preference. Pending
password/import/update/extension operations prevent a restart. Confirmations
unrelated to page closing are not silently accepted.

The manager distinguishes changes waiting for restart, a startup load request,
and activity actually observed from an extension context, target or popup.
A request to load is not displayed as proof of a running extension. An idle
extension can remain in the awaiting-activity state. After disabling or updating,
the old runtime can remain active until restart.

CEF retained an older MV3 service worker when replacing a package under the
same path. After signature verification, Aura therefore gives the worker entry
point a versioned sibling filename and adjusts the extracted manifest. The
JavaScript bytes and parent directory are preserved, including relative imports.
The extension ID and local storage remain the same. Tests exercise actual
1.0 → 2.0 → 3.0 module-worker execution and a value stored by version 1.0.

## Compatibility and data

Manifest V2 is rejected. MV3 content scripts, workers, messaging, storage,
declarativeNetRequest, popup and options pages have been exercised. The current
CEF OSR/Alloy engine does not expose Chrome's full tabs/window model or a public
API to hot-load and hot-unload these extensions. Full Web Store compatibility
is not claimed; extensions relying on unsupported APIs can work partially.
No remote debugging server is opened to manage extensions.

Extensions are enabled only for the normal profile. Private tabs cannot open
extension popups. Uninstall removes the package and registry entry; it does not
promise to erase all Chromium-managed extension data. Reinstalling the same ID
may reuse data. Use a separate profile for isolated testing.

## Verification

`ctest --preset linux-x64-release` covers signatures, archives, numeric version
ordering, optional/new permissions, cancellation, registry-write rollback,
worker preparation and process argument handling. The private CPU display suite
`python3 scripts/verify_browser.py --suite=extensions` exercises restart,
normal/private session policy, pinning, actual upgraded worker execution,
retained storage, downgrade refusal and disable/enable/remove. It uses signed
fictional fixtures and a local HTTP server, not a user's browser profile.
