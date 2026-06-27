# Repository Guidelines

## Project Structure & Module Organization

Taiko Zucchini is a C-based PS3 SPRX mod built from the repository root. Core runtime code lives in `core/`, with subsystem code split into `hooks/`, `patches/`, `storage/`, `network/`, `input/`, `qr/`, `cards/`, `mod_menu/`, `ftp/`, and `bpreader/`. Bootstrap and support executables are in `bootstrap_eboot/` and `ftp_eboot/`. EBOOT decrypt/patch/sign logic is under `eboot_patcher/`. Static assets live in `fonts/`; design and operational notes live in `docs/`. Vendored third-party code is in `vendor/` and should be changed only when intentionally updating that dependency.

## Build, Test, and Development Commands

- `nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token`: Windows build from a Visual Studio developer prompt; defaults to `H:\PS3_SDK` when `SCE_PS3_ROOT` is unset.
- `make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token`: GNU Make build for Linux, MSYS/Git Bash, or wrapper-based SDK setups.
- `make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token bootstrap`: builds only the first-run bootstrap EBOOT.
- `make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token ftp-eboot`: builds only the FTP EBOOT.
- `make install RPCS3_DEV_HDD0=/path/to/dev_hdd0 TAIKO_ZUCCHINI_API_TOKEN=token`: installs the SPRX into an RPCS3 plugin tree.
- `make clean`: removes generated objects and build outputs.

## Coding Style & Naming Conventions

Use C99/GNU99 conventions already present in the tree: four-space indentation, braces on function lines, `snake_case` functions and variables, and `TAIKO_` or subsystem-prefixed macros for constants. Keep headers paired with source files where practical. Prefer small `static` helpers for file-local behavior and keep public APIs explicit in headers. Follow existing defensive bounds checks for fixed-size buffers and PS3 SDK calls.

## Testing Guidelines

There is no standalone automated test suite. Validate changes with a full Makefile build and, when relevant, runtime checks on RPCS3, PS3, or S357 hardware. Targeted self-tests are gated by compile-time config such as `TAIKO_CAMERA_QR_SELFTEST`, `TAIKO_NETWORK_TLS_SELFTEST`, and `TAIKO_UPDATE_LOCAL_TEST`; keep these disabled for normal builds. Document the hardware/emulator version, game version, and observed logs when reporting validation.

## Commit & Pull Request Guidelines

Recent history uses short, direct summaries such as `fixed katsu don` and `updated readme`. Keep commits focused and describe the user-visible or runtime effect. Pull requests should include a concise summary, build command used, validation environment, relevant logs/screenshots for runtime behavior, and any proprietary files or keys required locally but not included.

## Security & Configuration Tips

Do not commit Sony SDK files, game dumps, signing keys, certificates, tokens, or generated binaries. The existing `.gitignore` excludes common proprietary and build artifacts; extend it before adding new local-only outputs.
