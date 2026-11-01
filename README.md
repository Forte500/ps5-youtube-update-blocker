<div align="center">

# youtube-update-blocker

**Stop YouTube from updating itself on a jailbroken PS5.**

A single ELF payload that patches the console's app database in place —
no SQLite engine, no intermediate app, no structural changes.

![Platform](https://img.shields.io/badge/platform-PlayStation%205-003791?logo=playstation&logoColor=white)
![Language](https://img.shields.io/badge/language-C11-3572A5)
![Payload](https://img.shields.io/badge/payload-ELF-lightgrey)
![Verified](https://img.shields.io/badge/verified-real%20DB%20dump-success)

</div>

---

## Overview

On the PS5, `/system_data/priv/mms/appinfo.db` is a SQLite database that
holds the metadata of every installed app. For YouTube it stores the
installed version and the URL the system queries for version information.

`youtube-update-blocker.elf` rewrites those two values:

| Key | Original | Blocked |
|---|---|---|
| `CONTENT_VERSION` | `01.000.003` | `99.999.999` |
| `VERSION_FILE_URI` | Sony update URL | `http://localhost` |

The console then believes YouTube is already at an unreachable version and
has no server to query for updates.

The payload is deliberately minimal: it does **not** embed a SQLite
engine. It walks the database b-tree, locates the live cells, and
overwrites only the bytes of the two values — keeping their length
identical, so no header, varint, cell or page ever changes.

---

## Build from source

You need a POSIX host (Linux or macOS) and the
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

### 1. Install the SDK prerequisites

<details open>
<summary><b>Debian / Ubuntu</b></summary>

```sh
sudo apt-get update
sudo apt-get install bash clang-18 lld-18 wget
```
</details>

<details>
<summary><b>Fedora</b></summary>

```sh
sudo dnf install bash llvm-devel clang lld wget
```
</details>

<details>
<summary><b>macOS (Homebrew)</b></summary>

```sh
brew install llvm@18 wget
export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config
```
</details>

### 2. Install the SDK

Download the latest binary distribution and unpack it to `/opt`:

```sh
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
```

This creates `/opt/ps5-payload-sdk`. If you prefer another directory,
build from the SDK sources instead (`sudo make DESTDIR=<dir> install`).

### 3. Point the build at the SDK

Either export the variable:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

or pass it to every `make` invocation:

```sh
make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

> The default is `$HOME/ps5-payload-sdk`, so if you unpacked it there
> nothing else is needed.

### 4. Build

```sh
make
```

Expected result:

```
/opt/ps5-payload-sdk/bin/prospero-clang -std=c11 -Wall -Wextra -O2 -g \
    -o youtube-update-blocker.elf src/main.c src/sqlite_patch.c \
    src/sha256.c src/notify.c -lSceNotification

Built youtube-update-blocker.elf
```

The payload is now in the project root.

### Troubleshooting

| Message | Fix |
|---|---|
| `PS5_PAYLOAD_SDK not found at "..."` | The path is wrong. Check that `<path>/toolchain/prospero.mk` exists. |
| `prospero-clang: No such file or directory` | The SDK is not installed or incomplete; re-download it. |
| `llvm-config: command not found` | Install `clang`/`lld` (step 1); on macOS set `LLVM_CONFIG`. |
| `undefined reference to 'sceNotificationSend'` | The SDK is incomplete or too old; use a current release. |

---

## How it works

```text
             appinfo.db
                 │
                 ▼
     ┌───────────────────────┐
     │ Validate SQLite       │
     │ header, pages, b-tree │
     └───────────┬───────────┘
                 │ fail ──► refuse, nothing written
                 │ pass
                 ▼
     ┌───────────────────────┐
     │ Back up to            │
     │ /data/appinfo.db.orig │
     └───────────┬───────────┘
                 ▼
     ┌───────────────────────┐
     │ Overwrite the two     │
     │ value fields in place │
     └───────────┬───────────┘
                 ▼
     ┌───────────────────────┐
     │ Re-read and verify    │
     │ SHA-256 on disk       │
     └───────────┬───────────┘
                 ▼
        Toast + reboot hint
```

1. **Validate** — checks the SQLite header, page size and declared page
   count, then parses `sqlite_master` to find the `tbl_appinfo` root page.
2. **Find** — walks the table b-tree down to the leaf cells and matches
   the YouTube title.
3. **Back up** to `/data/appinfo.db.orig` *before* touching anything.
4. **Patch** the `CONTENT_VERSION` and `VERSION_FILE_URI` value bytes in
   place. `http://localhost` is padded with spaces to the original
   255-byte length.
5. **Verify** by re-walking the patched tree and re-reading the file from
   disk; the on-disk SHA-256 must match what was written.
6. **Notify** with a system toast and log everything to
   `/data/appinfo-block.log`.

Because only value bytes change, the file stays a valid SQLite database:
`PRAGMA integrity_check` still returns `ok`.

---

## Regional variants

The payload has no hardcoded title id. It recognises YouTube from the
`CONTENT_ID` row containing `YOUTUBE`, and falls back to the known regional
title ids:

| Region | Title ID |
|---|---|
| US | `PPSA01650` |
| EU | `PPSA01651` |
| JP | `PPSA01652` |

The same ELF works everywhere. If more than one variant is installed, all
of them are blocked in a single pass.

---

## Safety model

Nothing is written unless every check passes:

- ✅ SQLite header, page size and page count are consistent
- ✅ no `-journal` or `-wal` file sits next to the database
- ✅ at least one YouTube title is found
- ✅ every recognised variant has exactly one `CONTENT_VERSION` and one
  `VERSION_FILE_URI` row
- ✅ `CONTENT_VERSION` is 10 bytes in `NN.NNN.NNN` form
- ✅ the URI value is at least 16 bytes long
- ✅ no matched row overflows its b-tree page

It is **idempotent**: if the database is already blocked it exits with
"already blocked" and writes nothing. It never reboots the console.

> A backup is always written before the first modification. If the backup
> fails, the database is left untouched.

---

## Usage

Load `youtube-update-blocker.elf` with a regular ELF loader (elfldr,
etaHEN, kstuff…). The process needs root privileges to write the database.

| Command | Effect |
|---|---|
| `youtube-update-blocker.elf` | analyse, back up, patch |
| `youtube-update-blocker.elf --dry-run` | analyse only, no writes |
| `youtube-update-blocker.elf --no-notify` | skip the system toast |
| `youtube-update-blocker.elf --path /data/appinfo.db` | use an alternate database |

Output goes to stdout and, in append mode, to `/data/appinfo-block.log`.
**Reboot the console** when the payload is done so the system re-reads the
database.

### Recommended first run

1. Copy `appinfo.db` from the console over FTP to `/data/appinfo.db`.
2. Run the payload in analysis mode:

   ```sh
   youtube-update-blocker.elf --dry-run --path /data/appinfo.db
   ```

   Check that it reports the expected title id, offsets and value lengths.
3. If everything looks right, run it without `--dry-run`.

---

## Restore

There is no automatic restore. Put the backup back and reboot:

```sh
put /data/appinfo.db.orig /system_data/priv/mms/appinfo.db
```

---

## What has been verified

The patcher has been tested against a real EU database dump and synthetic
US/JP databases:

- correct values after patching, unchanged file size
- **zero bytes changed outside the two value fields**
- `PRAGMA integrity_check` still `ok`, full row-by-row comparison
- idempotent second run
- refusal of non-SQLite, truncated, schema-less, unpatchable and
  overflowed databases

The system toast payload is validated as JSON before it is sent.

---

## Technical notes

**Dead cells.** The real database contains stale copies of the rows in the
free space of pages. A naive search-and-replace would patch those too.
This payload walks the b-tree instead and only touches live cells — in the
reference dump, exactly two, both on page 109.

**Fixed-width padding.** `VERSION_FILE_URI` is stored padded with spaces.
The replacement keeps the exact original length, so no record header has to
be rewritten.

**Why no SQLite engine.** Writing with SQLite on-device would need
`libsqlite3`, extra memory and trusted file locking. A byte-level patch of
two known cells is smaller and auditable.

---

## Limitations

- Requires a jailbroken console with root privileges.
- The byte-level approach assumes the two rows fit inside their b-tree page
  (the payload refuses otherwise).
- Verified against real dumps on the host; **tested on real 2 PS5 fat and one PS5 PRO**.
- YouTube may show an odd version in the store, and the official repair
  option may not work.

> Use it only on a console you own. Keep a copy of `appinfo.db.orig` outside
> the project folder too.

---

## Acknowledgements

- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) for the PS5 ELF
  toolchain and the notification sample.
- The Y2JB Installer project for documenting the original
  `appinfo.db` approach.
