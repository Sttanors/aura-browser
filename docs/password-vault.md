# Saved logins and encrypted transfer

Open **Settings → Data → Passwords**, or press **Ctrl+Shift+Y**. This feature is
currently in source builds; the old preview.3.1 installers do not contain it.

Create a master password with at least 12 characters. Aura does not store that
password or use the release-signing key. After restarting, unlock the vault again.
**Lock vault** immediately discards its in-memory entries and encryption key;
15 minutes without vault actions also locks it. A lost master password cannot be
recovered. Keep a separate, protected backup.

Use **Add login** for a site, username and password, or enter them on a web page
and choose **Save login from page**. Saving from a page shows a confirmation with
the site and username. **Edit selected login** can change all three fields;
a conflicting site/username is rejected. Deletion requires confirmation.
Password fields are masked and their copy/cut shortcuts are disabled.

To sign in, open the site's HTTPS login page, select its saved entry and choose
**Fill selected login**. Aura requires the exact origin (scheme, host and port).
It fills one visible main-frame login form whose action points to the same origin.
It never submits the form. HTTP pages, private tabs, iframe-only forms, ambiguous
password forms, password-creation fields and navigations during the request are
rejected. No background scraping or automatic filling happens. Custom forms,
shadow DOM, multi-step login pages without a password field and cross-origin
identity-provider forms may require entering credentials manually. This implements
login autofill, not address/payment-card autofill or passkey management.

## Transfer to another device

Choose **Transfer passwords**, enter a new absolute file path and a separate
transfer password, then **Export encrypted file**. Existing destination files are
never overwritten. Transfer that `.aura` file using your preferred local medium.
On the other device, create/unlock its vault and use **Import encrypted file**
with the transfer file's password. Linux and Windows use the same format.

Import merges entries by exact site and username. Existing local entries win
conflicts; no changes occur for duplicate-only imports. Before a changed import,
Aura saves the previous encrypted file next to the vault as
`passwords.aura.backup-TIMESTAMP`. That backup needs the master password it used
at the time, which may differ after a password change. Importing a corrupted file
or using a wrong password leaves the current vault unchanged.

This is a manual transfer of saved logins, as selected for this stage. It is not
continuous synchronization and does not transfer bookmarks, history, cookies,
settings, extensions or device sessions. Browser bookmark/history import is a
separate feature. No account, server, GitHub secret or network upload is involved.

## Storage and verification

`PROFILE/passwords.aura` contains authenticated AES-256-GCM ciphertext. Format 1
uses a random 16-byte scrypt salt, scrypt N=131072/r=8/p=1 and a 256-bit derived
key; each write has a new random 12-byte nonce and a 16-byte authentication tag.
The version header, salt and nonce are authenticated as additional data. Work
factors are fixed by the format, not supplied by an imported file. Payloads are
bounded to 16 MB and 10,000 entries. KDF/file work runs outside the UI thread.

Writes use a private, exclusively created sibling file, flush it, then rename it;
Linux files use mode 0600. Windows files inherit their destination folder's ACL.
A failed replacement preserves the previous file and the in-memory entries.
Do not place your live profile in an untrusted shared folder.

Aura clears owned secret strings/keys on lock, closes editor buffers when leaving
the panel, avoids plaintext temporary files, and excludes passwords from its
reports and logs. Unlocked values necessarily exist in process memory and in the
page after filling; the page can read its own form fields. This is not protection
against malware, process dumps, a compromised operating system or a malicious
site to which you explicitly fill a login. No independent security audit has been
performed. Chromium's separate saving and automatic-sign-in preferences are
disabled where those preferences are registered by CEF.

Tests use fictional credentials. C++ coverage checks tampering, wrong passwords,
nonce changes, locked access, write failures, editing conflicts, password changes,
worker completion, idle locking and encrypted import backups. The private CPU GUI
suite uses a local HTTPS server with an explicitly pinned test certificate; that
pin is supplied only to the fixture process and does not change Aura's defaults.
It exercises saving, filling, origin/HTTP/iframe/private rejection, restart,
transfer and password-free diagnostics.

References: [OpenSSL scrypt](https://docs.openssl.org/3.0/man7/EVP_KDF-SCRYPT/),
[OpenSSL authenticated encryption](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/),
[Chromium password preferences](https://chromium.googlesource.com/chromium/src/+/HEAD/components/password_manager/core/common/password_manager_pref_names.h).
