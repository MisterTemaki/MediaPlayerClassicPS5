# Credential vault (1.6.2)

Addresses shown by the app omit network passwords. Saved passwords are encrypted in `credentials.vault` rather
than written to `credentials.ini`. This implements [issue #7](https://github.com/MisterTemaki/MediaPlayerClassicPS5/issues/7).

## Using saved credentials

Add a network location with its user name and password as usual. The first time a password needs to be saved,
the app asks you to create a master password with at least eight characters and confirm it. Password fields
are masked. Creating or unlocking the vault runs in a background thread so the dialog can keep drawing.

The network user name and password are kept in the encrypted vault. You do not need to enter them for each file.
By default, the master password is requested at the first protected connection in each app session. It remains
unlocked until the app closes.

Open **View > Options > Miscellaneous > Credential vault...** to manage the vault. While it is unlocked, leave
the password fields empty to keep the current master password, or enter and confirm a new one to change it.

## Remember on this PS5

Tick **Remember on this PS5 (automatic unlock)** and choose **Save** to unlock the vault automatically on later
launches. You will not need to re-enter the network user name, network password or master password for those
launches. The option is off by default.

![Remembered unlock checkbox in the real dialog, captured on the Linux host](screenshots/credential-vault.png)

Automatic unlock stores a local key under `.credentials-device/unlock.key`, inside the app's data directory.
The directory uses mode `0700`, and the key file uses mode `0600`. The key is bound to the current vault salt
and verified by authenticated decryption. A stale, damaged or non-private key is rejected.

**Anyone who can read both this local key and the vault can decrypt the saved passwords.** The key is not
hardware-bound. These permissions do not protect it from another program with equivalent filesystem access.
The dialog explains this tradeoff before the checkbox is selected. Keep the option off when the master
password should protect a copied data folder.

To disable automatic unlock, open the same dialog, uncheck the option and choose Save. If the vault is already
unlocked, leave the password fields empty. Both the local key and a temporary key left by an interrupted save
are removed. The next app session requires the master password again.

## Migrating older installations

The app imports old passwords from URL fields and the legacy `credentials.ini` / `credentials.ini.new` pair.
Its generation marker selects the correct file when an old save was interrupted. Passwords are removed from
runtime model URLs immediately; new passwords are added to the vault before they reach saved addresses.

Legacy files are removed only after an encrypted vault has been written, synced, decrypted again for
verification and committed, followed by a successful write of the password-free settings. Cancelling setup or
failing to write the vault preserves the original files. Unreadable or malformed legacy data is kept and
blocks migration instead of being silently discarded.

Migration needs your master-password setup; cancelling leaves old plaintext on disk until setup is completed.
The app does not promise secure erasure of deleted files from SSDs or backups. Existing settings, media,
favorites and positions are retained. Older app versions do not understand the new vault format, so downgrading
may require entering network credentials again.

## Storage and cryptography

- `credentials.vault`: authenticated encrypted account/password records, including a versioned header, KDF
  parameters, a random 16-byte salt, a random 24-byte nonce and a 16-byte authentication tag.
- `.credentials-device/unlock.key`: optional local unlock material. It contains no master password or plaintext
  network password, but grants access to the vault and must be treated as sensitive.
- `mpc-hc.ini`: settings and password-free addresses. The master password is never written to it.

This vault handles passwords in URL userinfo, such as `smb://user:password@server/share`. Tokens in URL query
parameters or other application-specific authentication formats are outside its scope.

The implementation uses the existing, unchanged **Monocypher 4.0.2** dependency:
[Argon2id](https://monocypher.org/manual/argon2) with 64 MiB, three passes and one lane derives a 32-byte key;
[XChaCha20-Poly1305](https://monocypher.org/manual/aead) encrypts and authenticates the records and header.
Salt and nonce bytes come from the operating-system entropy backend already used by Mbed TLS. Entropy failures
stop the write. There is no hard-coded master key or plaintext fallback.

Writes use an owner-only temporary file, file sync and atomic rename. Normal vault temporary files contain
ciphertext. The explicitly opted-in automatic-unlock temporary file contains key material and is also private.
KDF work memory, session keys and password input buffers are wiped when no longer needed. Secret values still
exist in process memory while they are being used by the SMB transport. This change protects stored files;
it does not protect a running app from a process with access to its memory.

Deleting a network location does not erase the entire vault. If you intend to discard all saved credentials,
close the app and remove both `credentials.vault` and `.credentials-device`; keep a backup first if you still
need those credentials. Without the master password or a valid remembered key, the encrypted credentials cannot
be recovered. An already automatically unlocked session can set a new master password.

## Validation

- `make test-credentials`: cryptographic round trips, wrong passwords, modified/truncated files, bounded KDF
  parameters, failed writes, key rotation, private permissions and remembered-unlock enable/disable.
- `make test-credentials-ui`: real masked dialogs, confirmation, background KDF, migration/cancellation,
  checkbox persistence, URL rendering, nested prompts and translated layouts.
- Frontend group 19: authenticated SMB playback across app processes, remembered unlock, disabling it,
  legacy migration and generation recovery, plus the existing audio/HDR/decoder regressions.

The permission tests require a filesystem with POSIX permissions. A Windows-mounted filesystem without Unix
metadata cannot provide the private-key permissions; automatic unlock intentionally refuses that storage.
Host checks use simulated PS5 calls and do not replace a physical console test.

Validation of this change completed with **282 checks passed and no failures**:

| Checks | Passed |
| --- | ---: |
| Credential vault core, including corrupt data, failed writes and remembered keys | 40 |
| Credential dialogs, migration, nested prompts and translated layouts | 39 |
| Theme harness | 27 |
| Frontend groups 1, 2, 9, 17, 19 and 36, including authenticated SMB across processes | 93 |
| Existing update, log redaction, installer, GPU startup, decoder memory and HDR harnesses | 83 |

Clang 18 host runs used AddressSanitizer and UndefinedBehaviorSanitizer. The translation table passed
validation for 695 texts. The native PS5 GPU build and complete installer also passed, including import,
runtime-link, syscall and RADV linkage checks. The app version is `1.6.2`, content version `01.062.000`,
with the existing title ID `PPSA99015`. The full frontend suite was not rerun, and physical PS5 testing is
still pending.
