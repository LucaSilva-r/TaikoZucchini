# Standalone FTP EBOOT

This folder builds an FTP-only PS3 application. It is independent from
`zucchini.sprx` and does not load or patch any game EBOOT.

Build from the repository root:

```sh
scripts/build_sprx.sh ftp-eboot
```

Output:

- `TaikoZucchini/ftp_eboot/bin/ftp_eboot.elf`

Sign `bin/ftp_eboot.elf` with the SelfResigner tooling required for real
consoles, copy the resulting `EBOOT.BIN` into a bootable game/app `USRDIR`,
and launch it. The FTP server listens on port `2121`, starts in `/dev_hdd0`,
accepts anonymous login, and supports passive-mode transfers. The app
initializes a simple 720p status screen showing the IP address and port once
the network stack is ready.

Known limitation: a normal game process may be unable to create or modify
entries directly under `/dev_hdd0/game`. Writes inside existing game folders
can still work, but the parent directory itself is controlled by GameOS
permissions/database policy. This FTP EBOOT does not use kernel patches, so it
cannot bypass that restriction.
