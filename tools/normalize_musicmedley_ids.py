#!/usr/bin/env python3
"""Normalize Dani medley IDs between musicinfo.xml and musicmedleyinfo.xml.

Some dumped data has Momoiro-style medley IDs in musicmedleyinfo.xml
(`20000, 20002, 20004...`) while Kimidori's musicinfo.xml uses the sequential
medley rows that the game looks up (`20000, 20001, 20002...`). This tool keeps
the medley song contents intact and aligns the top-level medley IDs by order.
"""

from __future__ import annotations

import argparse
import re
import shutil
from datetime import datetime
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple


DATA_RE = re.compile(r"(<Data\b[^>]*>.*?</Data>)", re.DOTALL)
MEDLEY_RE = re.compile(
    r"(<MusicMedleyInfoData\b[^>]*>.*?</MusicMedleyInfoData>)",
    re.DOTALL,
)
MUSICID_RE = re.compile(r"<musicid>([^<]+)</musicid>")
UNIQUEID_RE = re.compile(r"(<uniqueid>)(\d+)(</uniqueid>)")
PARTSSET_RE = re.compile(r"<partsset>([^<]+)</partsset>")


Change = Tuple[int, str, str, str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Align musicmedleyinfo.xml top-level Dani medley IDs with "
            "musicinfo.xml medley rows."
        )
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--data-dir",
        type=Path,
        help="Directory containing musicinfo.xml and musicmedleyinfo.xml.",
    )
    source.add_argument(
        "--musicinfo",
        type=Path,
        help="Path to musicinfo.xml. Requires --musicmedleyinfo.",
    )
    parser.add_argument(
        "--musicmedleyinfo",
        type=Path,
        help="Path to musicmedleyinfo.xml when --musicinfo is used.",
    )
    parser.add_argument(
        "--start-id",
        type=int,
        default=None,
        help=(
            "Optionally renumber musicinfo.xml medley rows to this sequential "
            "starting ID before updating musicmedleyinfo.xml."
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report changes without writing or creating backups.",
    )
    parser.add_argument(
        "--backup-suffix",
        default=None,
        help="Backup suffix. Default: .bak-YYYYMMDD-HHMMSS",
    )
    return parser.parse_args()


def resolve_paths(args: argparse.Namespace) -> Tuple[Path, Path]:
    if args.data_dir is not None:
        if args.musicmedleyinfo is not None:
            raise SystemExit("--musicmedleyinfo cannot be combined with --data-dir")
        musicinfo = args.data_dir / "musicinfo.xml"
        medley = args.data_dir / "musicmedleyinfo.xml"
    else:
        if args.musicmedleyinfo is None:
            raise SystemExit("--musicinfo requires --musicmedleyinfo")
        musicinfo = args.musicinfo
        medley = args.musicmedleyinfo

    missing = [str(path) for path in (musicinfo, medley) if not path.is_file()]
    if missing:
        raise SystemExit("Missing required file(s): " + ", ".join(missing))
    return musicinfo, medley


def backup_file(path: Path, suffix: str | None) -> Path:
    if suffix is None:
        suffix = ".bak-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_name(path.name + suffix)
    if backup.exists():
        raise SystemExit(f"Backup already exists: {backup}")
    shutil.copy2(path, backup)
    return backup


def is_medley_musicinfo_block(block: str) -> bool:
    music_match = MUSICID_RE.search(block)
    if music_match is None:
        return False
    if not re.fullmatch(r"medley\d+", music_match.group(1).strip()):
        return False

    parts_match = PARTSSET_RE.search(block)
    return parts_match is None or parts_match.group(1).strip() == "dojo"


def rewrite_uniqueid(block: str, new_id: str) -> Tuple[str, str]:
    unique_match = UNIQUEID_RE.search(block)
    if unique_match is None:
        raise SystemExit("Medley block without <uniqueid>")

    old_id = unique_match.group(2)
    if old_id == new_id:
        return block, old_id

    rewritten = (
        block[: unique_match.start()]
        + unique_match.group(1)
        + new_id
        + unique_match.group(3)
        + block[unique_match.end() :]
    )
    return rewritten, old_id


def collect_musicinfo_targets(
    text: str,
    start_id: int | None,
) -> Tuple[str, List[str], List[Change]]:
    chunks: List[str] = []
    targets: List[str] = []
    changes: List[Change] = []
    cursor = 0
    medley_index = 0

    for match in DATA_RE.finditer(text):
        chunks.append(text[cursor : match.start()])
        block = match.group(1)

        if not is_medley_musicinfo_block(block):
            chunks.append(block)
            cursor = match.end()
            continue

        musicid = MUSICID_RE.search(block).group(1).strip()
        target = str(start_id + medley_index) if start_id is not None else None
        if target is None:
            unique_match = UNIQUEID_RE.search(block)
            if unique_match is None:
                raise SystemExit(f"musicinfo medley block without uniqueid: {musicid}")
            target = unique_match.group(2)
            chunks.append(block)
        else:
            block, old_id = rewrite_uniqueid(block, target)
            if old_id != target:
                changes.append((medley_index, musicid, old_id, target))
            chunks.append(block)

        targets.append(target)
        medley_index += 1
        cursor = match.end()

    chunks.append(text[cursor:])
    if not targets:
        raise SystemExit("No musicinfo medley rows found")
    return "".join(chunks), targets, changes


def rewrite_musicmedleyinfo(text: str, targets: Sequence[str]) -> Tuple[str, List[Change]]:
    chunks: List[str] = []
    changes: List[Change] = []
    cursor = 0
    medley_index = 0

    for match in MEDLEY_RE.finditer(text):
        chunks.append(text[cursor : match.start()])
        block = match.group(1)
        if medley_index >= len(targets):
            raise SystemExit(
                "musicmedleyinfo.xml has more MusicMedleyInfoData rows than "
                "musicinfo.xml medley rows"
            )

        name_match = re.search(r"<medleyname>([^<]+)</medleyname>", block)
        label = name_match.group(1).strip() if name_match else f"row{medley_index}"
        block, old_id = rewrite_uniqueid(block, targets[medley_index])
        if old_id != targets[medley_index]:
            changes.append((medley_index, label, old_id, targets[medley_index]))

        chunks.append(block)
        medley_index += 1
        cursor = match.end()

    chunks.append(text[cursor:])
    if medley_index != len(targets):
        raise SystemExit(
            "musicinfo.xml medley row count does not match musicmedleyinfo.xml "
            f"({len(targets)} vs {medley_index})"
        )
    return "".join(chunks), changes


def summarize(title: str, changes: Iterable[Change]) -> None:
    changes = list(changes)
    print(f"{title}: {len(changes)}")
    for index, label, old_id, new_id in changes[:40]:
        print(f"  {index:02d} {label}: {old_id} -> {new_id}")
    if len(changes) > 40:
        print(f"  ... {len(changes) - 40} more")


def main() -> int:
    args = parse_args()
    musicinfo, medley = resolve_paths(args)

    musicinfo_text = musicinfo.read_text(encoding="utf-8")
    medley_text = medley.read_text(encoding="utf-8")

    new_musicinfo, targets, musicinfo_changes = collect_musicinfo_targets(
        musicinfo_text,
        args.start_id,
    )
    new_medley, medley_changes = rewrite_musicmedleyinfo(medley_text, targets)

    print(f"musicinfo: {musicinfo}")
    print(f"musicmedleyinfo: {medley}")
    print(f"target medley IDs: {targets[0]}..{targets[-1]} ({len(targets)} rows)")
    summarize("musicinfo changes", musicinfo_changes)
    summarize("musicmedleyinfo changes", medley_changes)

    if not musicinfo_changes and not medley_changes:
        print("no write needed")
        return 0
    if args.dry_run:
        print("dry run; no files written")
        return 0

    suffix = args.backup_suffix
    musicinfo_backup = None
    medley_backup = None
    if musicinfo_changes:
        musicinfo_backup = backup_file(musicinfo, suffix)
        musicinfo.write_text(new_musicinfo, encoding="utf-8", newline="")
    if medley_changes:
        medley_backup = backup_file(medley, suffix)
        medley.write_text(new_medley, encoding="utf-8", newline="")

    if musicinfo_backup is not None:
        print(f"musicinfo backup: {musicinfo_backup}")
    if medley_backup is not None:
        print(f"musicmedleyinfo backup: {medley_backup}")
    print("written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
