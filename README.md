# Taiko Zucchini

> 🇯🇵 日本語？ → **[日本語版 README はこちら](README.ja.md)**

Taiko Zucchini is a PS3 SPRX mod for the Taiko no Tatsujin arcade games (Katsu
Don through Green) on Namco System 357. It installs as a runtime plugin and
uses a first-run bootstrap
EBOOT to patch the game's original EBOOT on the console.

> ## 📖 READ THIS WHOLE README CAREFULLY, FIRST
> Most install failures are people skipping steps. Read everything before you
> touch anything — especially the **PERMISSIONS** and **Before You Start**
> sections.

## Features

| Feature | What it does |
|---------|--------------|
| Dongle and VU patching | Lets the game pass the arcade USB dongle and VU storage checks without the original hardware. |
| Chassisinfo redirect | Generates and redirects chassis information so the game can pass one of Taiko's more painful startup checks. |
| EBOOT self-patching | Patches and re-signs the original EBOOT on first boot, then keeps the patched EBOOT up to date when the mod changes. |
| Modern HTTPS support | Replaces parts of the game's old network path with an mbedTLS-backed client for modern TLS endpoints. |
| Online redirect hooks | Provides HTTP, DNS, and socket hooks for private server routing and diagnostics. |
| DualShock support | Maps controller input into the game's USIO-style input path. |
| Keyboard support | Adds keyboard input for gameplay and operator actions. |
| QR and camera support | Adds camera hooks and QR scanning support for Banapass-style card workflows. |
| Mod menu | Provides an in-game configuration menu for common runtime options. |
| FTP server | Starts an operator FTP server from the mod menu for easier file access. |
| Overlay notifications | Shows runtime overlay messages, including update prompts, without relying on CFW-only notification APIs. |

## Compatibility

Taiko Zucchini supports the range from **Taiko no Tatsujin Katsu Don** up
through **Taiko no Tatsujin Green** (Katsu Don, Sorairo, Momoiro, Kimidori,
Murasaki, Blue/White, Green, etc.).

Do not install this release on a Taiko version outside that range unless
support is explicitly documented. The patcher depends on version-specific
EBOOT layout and patch offsets.

### Multiple versions at once

You can install **multiple Taiko versions side by side**. Each lives in its
own game folder (e.g. `SCEEXE001`, plus more for Green, White, Murasaki,
whatever). Zucchini patches each one independently, as long as each game
folder has the bootstrap EBOOT and the rest of its files in place.

The only requirement is that **permissions are correct on every game folder**
(see the PERMISSIONS section). Get that right and any number of versions can
coexist.

## Platform Requirements

On retail PS3 consoles, Taiko Zucchini requires custom firmware.

On original Namco System 357 hardware, the system needs the DEX firmware
modules installed so the bootstrap and SPRX loading flow can run.

## Before You Start

### No USB sticks

Make sure **no USB stick is plugged into the PS3** while running the mod —
not even an empty one. Any USB stick can confuse the dongle/VU detection.
Everything else (controller, etc.) is fine; just no USB storage.

### RPCS3: clean the USB virtual file system

On RPCS3 the same rule applies, plus the emulated USB devices must be cleaned
up. Open **Configuration → Virtual File System → dev_usb** and remove all
Vendor ID, Product ID, and Serial entries so every `/dev_usbNNN` row is blank
(only `/dev_usb000` keeps its default path). It should look like this:

```text
Device        Path                          Vendor ID   Product ID   Serial
/dev_usb000   $(EmulatorDir)dev_usb000/
/dev_usb001
/dev_usb002
/dev_usb003
...
```

If any VID/PID/Serial is set, **older Taiko versions WILL have issues.** Clear
them and click **Save**.

## Release Contents

A normal release should include:

- `zucchini.sprx`: the runtime plugin.
- `EBOOT.BIN`: a throwaway bootstrap EBOOT used for the first patch.

The release does not include game files, Sony SDK files, signing keys, app
loader keys, or other proprietary key material.

## Configuration

Configuration is **global** and lives next to `zucchini.sprx`:

```text
/dev_hdd0/plugins/taiko/taiko_config.cfg
/dev_hdd0/plugins/taiko/cards.cfg
```

- `taiko_config.cfg`: main config, shared by every game. A per-game overlay
  can override individual values, but the base config is global.
- `cards.cfg`: card registration data, also stored here.

Both files are created with sane defaults on first run if missing.

## ⚠️ PERMISSIONS — READ THIS OR NOTHING WORKS ⚠️

> **EVERY file and folder under `/dev_hdd0/plugins/taiko/` AND under your
> game folder `/dev_hdd0/game/<game folder>/` MUST be set to `777`
> permissions, owned by `root`, RECURSIVELY.**
>
> This is the single most common cause of failure. If permissions or owner
> are wrong, the mod will fail to read/write configs, patch the EBOOT, or
> load — often with confusing or silent errors.

Correct order of operations:

1. Copy **everything** over first — both the game and the mod files.
2. Set up all the files (configs, keys, EBOOT rename, DATA00000.BIN, etc.).
3. **THEN**, as the last step, set permissions and owner recursively.

Two ways to set permissions/owner:

- **FTP** — only works on CFW retail consoles and consoles with HEN.
  Use an FTP client (or the built-in FTP server, see below) with an account
  that can `chmod`/`chown` to apply `777` + `root` recursively.
- **Virtual machine / direct HDD editing** — mount the PS3 HDD directly and
  fix permissions there. This is the **only** option on S357 hardware, since
  those cannot currently be modded over the network.

Apply recursively to both trees, e.g.:

```sh
chmod -R 777 /dev_hdd0/plugins/taiko
chown -R root:root /dev_hdd0/plugins/taiko
chmod -R 777 /dev_hdd0/game/<game folder>
chown -R root:root /dev_hdd0/game/<game folder>
```

## Installation

1. On the PS3 hard drive, create this folder:

   ```text
   /dev_hdd0/plugins/taiko/
   ```

2. Copy `zucchini.sprx` from the release into that folder:

   ```text
   /dev_hdd0/plugins/taiko/zucchini.sprx
   ```

3. In the same folder, create a `keys` folder:

   ```text
   /dev_hdd0/plugins/taiko/keys/
   ```

4. Copy your own signing key files into the `keys` folder. The
   patcher expects the files to be named `keys` and `ldr_curves`:

   ```text
   /dev_hdd0/plugins/taiko/keys/keys
   /dev_hdd0/plugins/taiko/keys/ldr_curves
   ```

   These files are required so the mod can decrypt, patch, and re-sign the
   original EBOOT. They are not distributed with Taiko Zucchini.

5. Open the Taiko Green game `USRDIR` folder.

6. Rename the original, unpatched, signed game EBOOT:

   ```text
   EBOOT.BIN -> EBOOT_ORIGINAL.BIN
   ```

7. Copy the release-provided bootstrap `EBOOT.BIN` into the same `USRDIR`
   folder, replacing the original filename.

After these steps, the important files should be arranged like this:

```text
/dev_hdd0/plugins/taiko/
  zucchini.sprx
  keys/

<Taiko Green USRDIR>/
  EBOOT.BIN
  EBOOT_ORIGINAL.BIN
```

8. Copy the DATA00000.BIN file from the usb stick (usually found in /dev_usb000/VERSIONUP) into the USRDIR folder
```text
/dev_hdd0/plugins/taiko/
  zucchini.sprx
  keys/

<Taiko Green USRDIR>/
  EBOOT.BIN
  EBOOT_ORIGINAL.BIN
  DATA00000.BIN
```

## First Boot

The release `EBOOT.BIN` is only a bootstrap. Its job is to load:

```text
/dev_hdd0/plugins/taiko/zucchini.sprx
```

On first boot, the SPRX finds `EBOOT_ORIGINAL.BIN`, patches it, re-signs the
patched result, and writes the real patched `EBOOT.BIN` back into `USRDIR`.
The bootstrap EBOOT is moved aside during this process.

When the first patch finishes, power-cycle or reboot the game. The next boot
uses the patched game EBOOT directly, and the bootstrap EBOOT is no longer
needed for normal startup.

Keep `EBOOT_ORIGINAL.BIN` in place. Future Taiko Zucchini updates may use it
to regenerate the patched EBOOT when the patcher changes.

## Preboot Mod Menu

The mod menu opens during the early preboot window.

- On a DualShock controller, hold **L3 + R3** for about 3 seconds while the
  game is starting.
- On a keyboard, repeatedly tap **F2** until the mod menu opens. Holding F2
  before the keyboard is initialized may not register, so tap it a few times.

## Updating

To update Taiko Zucchini, replace:

```text
/dev_hdd0/plugins/taiko/zucchini.sprx
```

The patched EBOOT loads the SPRX from that path. If the new SPRX needs a new
EBOOT patch, it can repatch from `EBOOT_ORIGINAL.BIN`.

## S357 (GEX) Setup Guide

S357 hardware cannot currently be modded over the network. To get the mod
loading you must convert the unit to a "D-GEX" hybrid firmware so it has full
XMB access, then install the game. Everything is done by mounting the PS3 HDD
directly (VM or a Linux box).

> ⚠️ This wipes/reinitializes your drive. Make a backup first (step 1). After
> all files are in place, remember the **PERMISSIONS** section above — `777` +
> `root`, recursive, on both `plugins/taiko` and the game folder.

### Requirements

1. RPCS3 installed on your computer.
2. A VM or Linux install for mounting the S357 HDD — if using a VM, ideally a
   separate PC running Linux.
3. At least 250 GB of free space.
4. Hard drive access to your S357.
5. A USB stick with at least 256 MB free.
6. A compatible PlayStation 3 controller and a Mini-USB cable for it. Generic
   pads (DS4 / DualSense / Switch Pro Controller) work for gameplay but will
   **not** let you exit back to XMB, due to how the PS3 OS handles them.

### Prerequisites

1. Create an `.img` backup of your S357 HDD in its current state using `dd` on
   Linux. This is your restore point if anything goes wrong or you want to
   revert.
2. After backing up, intentionally initialize your drive as GPT or MBR. This
   forces the S357 into Safe Mode so you can do a clean install of GEX.
3. Download 4.70 GEX:
   <https://archive.midnightchannel.net/SonyPS/Firmware/download/5addd20173bfb15b6e18461b8f928027/GEX_CRC%5B476E8B6D%5D_FW%5Bv4.70%5D_PS3UPDAT.PUP>
4. Rename the file to `PS3UPDAT.PUP` and place it on an MBR, FAT32-formatted
   USB drive at: `PS3/UPDATE`.
5. Plug the USB into the S357 and follow the process to reinstall GEX.
6. Once booted into GEX 4.70, wait for initial setup to finish (it will throw
   an error and reboot — this is expected), then unplug your S357.

### Setting up "D-GEX" hybrid firmware

1. Download 4.70 DEX:
   <https://archive.midnightchannel.net/SonyPS/Firmware/download/b74627dadcef86ebff0c2a424106ec4d/DEX_CRC%5B1E5390FD%5D_FW%5Bv4.70%5D_PS3UPDAT.PUP>
2. Extract the `PUP` using RPCS3 (Utilities → Extract PUP).
3. Extract the `Encrypted TAR` using RPCS3 (Utilities → Extract Encrypted TAR).
4. Mount the S357 HDD via Linux.
5. Open `dev_flash` as **root**.
6. Delete all files in `dev_flash`.
7. Copy each folder from the DEX `dev_flash` to the S357 HDD one by one,
   **starting with the largest folders**. It has to be done this way or you
   WILL hit errors, due to how UFS2 handles remaining storage.
8. Unmount.
9. Boot the S357 and enjoy full XMB access. It will run you through the regular
   PS3 setup.

### Installing Taiko Green (or another version)

1. Get a copy of Taiko Green 13.02 (your own dump, or elsewhere).
2. Rename `SCEEXE000` to `SCEEXE001`.
3. Open `PARAM.SFO` in a hex editor (HxD, ImHex, etc.) and change `SCEEXE000`
   to `SCEEXE001` in the file.
4. Mount the S357 HDD via Linux.
5. Open `dev_hdd` as **root**.
6. Navigate to `game`.
7. Delete `SCEEXE000` — this is a stub generated on the initial boot of the GEX
   reinstall.
8. Copy `SCEEXE001` into `game`.
9. Once copying finishes, delete `metadata_db_hdd` in `dev_hdd/mms/db` to
   trigger a database restore. This makes Taiko Green show up on the XMB under
   its custom Title ID and removes the stub set up by GEX.

After this, follow the normal **Installation** steps to add the mod, then apply
the **PERMISSIONS** fixes.

## Build Notes

This project requires a local Sony Cell SDK installation. The SDK, game
binaries, and private key material are not included and must not be committed.

Prepare a Cell SDK tree and point `CELL_SDK` at it. On Windows, the expected
SDK layout is the native `host-win32/` toolchain:

```text
<CELL_SDK>/host-win32/ppu/bin/ppu-lv2-gcc.exe
<CELL_SDK>/host-win32/spu/bin/spu-lv2-gcc.exe
<CELL_SDK>/host-win32/bin/ppu-lv2-prx-strip.exe
<CELL_SDK>/host-win32/bin/make_fself.exe
<CELL_SDK>/target/ppu/
<CELL_SDK>/target/spu/
<CELL_SDK>/target/common/
```

### Windows with Visual Studio NMAKE

Open an x64 Native Tools Command Prompt for Visual Studio, or initialize the
environment from your installed Visual Studio first, then build from the
repository root:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=your_token
```

`Makefile.win` defaults to `%SCE_PS3_ROOT%` when set, otherwise `H:\PS3_SDK`.
Override it when needed:

```bat
nmake /f Makefile.win CELL_SDK=H:\PS3_SDK TAIKO_ZUCCHINI_API_TOKEN=your_token
```

Useful Windows targets:

```bat
nmake /f Makefile.win bootstrap
nmake /f Makefile.win ftp-eboot
nmake /f Makefile.win clean
```

### GNU Make

The original `Makefile` remains available for GNU Make. It auto-picks
`host-win32/` when present, otherwise `host-linux/`, and can still be used from
Linux, MSYS, Git Bash, or a Wine-wrapper SDK:

```sh
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=your_token
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=your_token bootstrap
```

The default build produces:

```text
bin/zucchini.sprx
bootstrap_eboot/bin/eboot.elf
ftp_eboot/bin/ftp_eboot.elf
```

`bootstrap_eboot/bin/eboot.elf` is an unsigned first-run bootstrap EBOOT. If
you build it yourself, sign it externally with your existing SELF signing
workflow before installing it as `EBOOT.BIN`.

## Repository Contents

- `core/`, `hooks/`, `patches/`, `storage/`, `network/`, `input/`, `qr/`:
  project source.
- `bootstrap_eboot/`: first-run bootstrap EBOOT source.
- `eboot_patcher/`: original EBOOT decrypt, patch, re-sign, and swap flow.
- `vendor/mbedtls/`: Mbed TLS source, under its upstream license.
- `vendor/quirc/`: quirc source, under its upstream license.
- `fonts/Roboto-Medium.ttf`: Roboto font, under the Apache License 2.0.
- `Makefile`: GNU Make Cell SDK SPRX build.
- `Makefile.win`: Visual Studio NMAKE build using the SDK `host-win32/` tools.

## Legal And Keys

Bring your own legally obtained game files and key material. Taiko Zucchini
does not distribute the Taiko game EBOOT, Sony SDK files, app loader keys, or other proprietary files.

Project code is released under the MIT license. Vendored dependencies and font
assets keep their own licenses; see `THIRD_PARTY_NOTICES.md`.
