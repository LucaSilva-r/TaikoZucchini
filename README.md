# Taiko Zucchini

Taiko Zucchini is a PS3 SPRX mod for Taiko no Tatsujin Green on Namco
System 357. It installs as a runtime plugin and uses a first-run bootstrap
EBOOT to patch the game's original EBOOT on the console.

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

**Big disclaimer:** Taiko Zucchini currently works only with **Taiko no
Tatsujin Green v11rXX**, **Taiko no Tatsujin Blue v10rXX**.

Do not install this release on another Taiko version unless support for that
version is explicitly documented. The patcher depends on version-specific
EBOOT layout and patch offsets.

## Platform Requirements

On retail PS3 consoles, Taiko Zucchini requires custom firmware.

On original Namco System 357 hardware, the system needs the DEX firmware
modules installed so the bootstrap and SPRX loading flow can run.

## Release Contents

A normal release should include:

- `zucchini.sprx`: the runtime plugin.
- `EBOOT.BIN`: a throwaway bootstrap EBOOT used for the first patch.

The release does not include game files, Sony SDK files, signing keys, app
loader keys, or other proprietary key material.

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

## Build Notes

This project requires a local Sony Cell SDK installation. The SDK, game
binaries, and private key material are not included and must not be committed.

Prepare a Cell SDK tree and point `CELL_SDK` at it. The Makefile expects the
SDK to provide these tools and directories:

```text
<CELL_SDK>/host-linux/ppu/bin/ppu-lv2-gcc
<CELL_SDK>/host-linux/spu/bin/spu-lv2-gcc
<CELL_SDK>/host-linux/bin/ppu-lv2-prx-strip
<CELL_SDK>/host-linux/bin/make_fself
<CELL_SDK>/target/ppu/
<CELL_SDK>/target/spu/
<CELL_SDK>/target/common/
```

If your SDK only has Windows-hosted tools under `host-win32/`, create your own
`host-linux/` wrapper layer that invokes those `.exe` tools through Wine, then
use that prepared SDK path as `CELL_SDK`.

Build from the Taiko Zucchini repository root:

```sh
make CELL_SDK=/path/to/cell
```

The default `all` target builds both release artifacts:

```text
bin/zucchini.sprx
bootstrap_eboot/bin/eboot.elf
```

To build only the bootstrap EBOOT:

```sh
make CELL_SDK=/path/to/cell bootstrap
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
- `Makefile`: Cell SDK SPRX build.

## Legal And Keys

Bring your own legally obtained game files and key material. Taiko Zucchini
does not distribute the Taiko game EBOOT, Sony SDK files, app loader keys, or other proprietary files.

Project code is released under the MIT license. Vendored dependencies and font
assets keep their own licenses; see `THIRD_PARTY_NOTICES.md`.
