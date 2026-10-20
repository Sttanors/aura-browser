# Signed browser updates

Aura reads the repository's public releases index and downloads `aura-update.toml`
and its detached `aura-update.toml.sig` from the selected tag. Preview builds include
prereleases; stable builds skip them. Both metadata requests are pinned to the same
tag, whose identity must match the signed payload. Only Ed25519 signatures
matching the public identity compiled into `common/update_key.hpp` are accepted.
The signature covers the exact TOML bytes, including installer SHA-256, byte size,
platform, installation kind, release sequence and expiry. Arbitrary download URLs
are not accepted. The application constructs URLs within its own release repository.

Checks run at startup at most once per day, can be disabled in General settings,
and are skipped on private startup. Downloads and installation require explicit
buttons in General → About Aura and updates. Network failures leave the running
browser untouched. Requests omit stored credentials, have size limits and deadlines;
installer bytes are streamed to a private staging directory and hashed off the UI thread.
Cancellation removes incomplete downloads. Signed metadata cannot roll back below
the highest sequence previously seen by that profile or the compiled sequence.

For a `.run` or Windows Setup installation, Aura verifies the downloaded installer,
saves the session and shuts Chromium down before launching it. Portable users get
the verified ZIP in a folder. `/opt/aura-browser` installations must be upgraded by
apt/dnf/pacman; Aura does not write into their managed runtime. There is no unattended
installation. This is channel authentication, **not Authenticode or a trusted Windows
publisher certificate**.

## Release procedure

1. Set `browser_version` and monotonically increase `browser_release_sequence` in
   `common/update_key.hpp`. Build and validate installers for that exact source.
2. Publish the completed non-draft GitHub release. Mark preview releases as
   prereleases so stable builds do not offer them.
3. Keep the private Ed25519 PEM local. The initial private key is in ignored
   `local/release-signing/channel.pem`, mode 0600; keep an offline backup.
   It is not part of Git, runtime archives, installers or GitHub Actions secrets.
4. Run `python scripts/sign_release.py --tag VERSION_TAG --key KEY_PATH` locally.
   It downloads the actual public installers, hashes and signs them, and uploads
   only the two public channel metadata files. `scripts/sign_update.py` can also
   sign explicitly selected local artifacts. **Verify update channel** in Actions
   uses only the public key and re-downloads/hashes every asset to verify the release.
5. Test an older installed build upgrading to this release, including profile
   retention, cancellation and a corrupted download. Signing tests alone do not
   prove an entire cross-platform upgrade lifecycle.

Metadata expires after 90 days. Re-sign unchanged assets before expiry, keeping the
same sequence. A different key is rejected by existing builds: changing channel
identity needs a deliberately staged trust migration, not replacing the header alone.

## Chromium maintenance

`CEF security maintenance` checks the official CEF index twice a week and on demand,
comparing numeric stable Chromium versions for both platforms. It fails when a newer
stable runtime exists. A maintainer still needs to update locked URLs/hashes and run
Linux and Windows build/integration checks. It does not silently replace CEF binaries.

## Scope still requiring release validation

The first published signed channel, real old-to-new upgrades, Windows installer
launch/closure and package-manager upgrades must be recorded in the progress journal.
They must not be represented as passing merely because unit tests passed.

## Reproduce the Linux installed-upgrade test

With the current signed release metadata in `artifacts/channel-current` and its
verified portable archive in `artifacts/aura2-linux64.zip`, run
`python scripts/prepare_update_journey.py`, then
`python scripts/verify_browser.py --suite=update-install`.
The preparation builds an isolated copy with sequence N−1 and the same public key;
it does not change repository sources or copy private signing material. The test
installs into its own HOME under `artifacts/updates-journey`, uses a private CPU
display, downloads the actual signed `.run` and checks replacement and profile
retention. This is a synthetic previous-version fixture, not a claim that historical
updater-less releases can update themselves.
