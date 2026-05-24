# Taiko Zucchini

Taiko Zucchini is a PS3 SPRX plugin project for runtime patching and diagnostics.
This repository starts at the former `sprx_plugin/` source tree so the public
history does not include private repository artifacts, proprietary SDK files, or
game dumps.

## Repository Contents

- `core/`, `hooks/`, `patches/`, `storage/`, `network/`, `input/`, `qr/`:
  project source.
- `vendor/mbedtls/`: Mbed TLS source, under its upstream license.
- `vendor/quirc/`: quirc source, under its upstream license.
- `fonts/Roboto-Medium.ttf`: Roboto font, under the Apache License 2.0.
- `bootstrap_eboot/`: first-run bootstrap EBOOT source.
- `Makefile`: Cell SDK SPRX build.

## Build

This project requires a local Sony Cell SDK installation. The SDK and game
binaries are not included and must not be committed.

```sh
make CELL_SDK=/path/to/cell
```

The build produces `bin/zucchini.sprx`.

It also produces `bootstrap_eboot/bin/eboot.elf`, an unsigned first-run
bootstrap EBOOT. Sign it externally with your existing SELF signing workflow
before installing it as `EBOOT.BIN`.

## Release Hygiene

The repository intentionally ignores generated PRX/SPRX/ELF/object outputs,
local SDK directories, private keys, certificates, and game dump formats.

## License

Project code is released under the MIT license. Vendored dependencies and font
assets keep their own licenses; see `THIRD_PARTY_NOTICES.md`.
