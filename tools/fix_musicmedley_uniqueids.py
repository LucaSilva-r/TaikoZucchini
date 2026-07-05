#!/usr/bin/env python3
"""Fix musicmedleyinfo.xml unique IDs by matching musicid in musicinfo.xml.

The medley XML sometimes contains stale uniqueid values. This script treats
musicid as the ground truth, looks up each song's uniqueid in musicinfo.xml,
then rewrites only the affected <uniqueid> values inside <Content> blocks.
Before writing, it creates a timestamped backup beside musicmedleyinfo.xml.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Tuple
from xml.etree import ElementTree


CONTENT_RE = re.compile(r"(<Content\b[^>]*>.*?</Content>)", re.DOTALL)
MUSICID_RE = re.compile(r"<musicid>([^<]+)</musicid>")
UNIQUEID_RE = re.compile(r"(<uniqueid>)(\d+)(</uniqueid>)")


Change = Tuple[str, str, str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Rewrite musicmedleyinfo.xml Content uniqueid values from "
            "musicinfo.xml, keyed by musicid."
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
        "--dry-run",
        action="store_true",
        help="Report changes without writing or creating a backup.",
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


def strip_namespace(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def load_musicinfo_uniqueids(path: Path) -> Dict[str, str]:
    mapping: Dict[str, str] = {}
    duplicates: Dict[str, List[str]] = {}

    for _event, elem in ElementTree.iterparse(path, events=("end",)):
        if strip_namespace(elem.tag) != "Data":
            continue

        musicid = None
        uniqueid = None
        for child in elem:
            name = strip_namespace(child.tag)
            if name == "musicid":
                musicid = (child.text or "").strip()
            elif name == "uniqueid":
                uniqueid = (child.text or "").strip()

        if musicid and uniqueid:
            previous = mapping.get(musicid)
            if previous is not None and previous != uniqueid:
                duplicates.setdefault(musicid, [previous]).append(uniqueid)
            mapping[musicid] = uniqueid

        elem.clear()

    if duplicates:
        lines = [
            f"{musicid}: {', '.join(values)}"
            for musicid, values in sorted(duplicates.items())
        ]
        raise SystemExit(
            "musicinfo.xml has conflicting uniqueid values for musicid:\n"
            + "\n".join(lines)
        )

    if not mapping:
        raise SystemExit(f"No musicid/uniqueid entries found in {path}")

    return mapping


def rewrite_medley_text(text: str, uniqueids: Dict[str, str]) -> Tuple[str, List[Change], List[str]]:
    changes: List[Change] = []
    missing: List[str] = []
    chunks: List[str] = []
    cursor = 0

    for match in CONTENT_RE.finditer(text):
        chunks.append(text[cursor : match.start()])
        block = match.group(1)
        music_match = MUSICID_RE.search(block)
        unique_match = UNIQUEID_RE.search(block)

        if music_match is None or unique_match is None:
            chunks.append(block)
            cursor = match.end()
            continue

        musicid = music_match.group(1).strip()
        current = unique_match.group(2)
        expected = uniqueids.get(musicid)

        if expected is None:
            missing.append(musicid)
            chunks.append(block)
            cursor = match.end()
            continue

        if current != expected:
            block = (
                block[: unique_match.start()]
                + unique_match.group(1)
                + expected
                + unique_match.group(3)
                + block[unique_match.end() :]
            )
            changes.append((musicid, current, expected))

        chunks.append(block)
        cursor = match.end()

    chunks.append(text[cursor:])
    return "".join(chunks), changes, missing


def backup_file(path: Path, suffix: str | None) -> Path:
    if suffix is None:
        suffix = ".bak-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_name(path.name + suffix)
    if backup.exists():
        raise SystemExit(f"Backup already exists: {backup}")
    shutil.copy2(path, backup)
    return backup


def summarize(changes: Iterable[Change], missing: Iterable[str]) -> None:
    changes = list(changes)
    missing_counts = Counter(missing)
    print(f"changed entries: {len(changes)}")
    for musicid, old, new in changes[:30]:
        print(f"  {musicid}: {old} -> {new}")
    if len(changes) > 30:
        print(f"  ... {len(changes) - 30} more")

    if missing_counts:
        print(f"missing musicinfo entries: {sum(missing_counts.values())}")
        for musicid, count in missing_counts.most_common(30):
            print(f"  {musicid}: {count}")
        if len(missing_counts) > 30:
            print(f"  ... {len(missing_counts) - 30} more")


def main() -> int:
    args = parse_args()
    musicinfo, medley = resolve_paths(args)

    uniqueids = load_musicinfo_uniqueids(musicinfo)
    original = medley.read_text(encoding="utf-8")
    rewritten, changes, missing = rewrite_medley_text(original, uniqueids)

    print(f"musicinfo: {musicinfo}")
    print(f"musicmedleyinfo: {medley}")
    summarize(changes, missing)

    if missing:
        print("error: not all medley musicid values exist in musicinfo.xml", file=sys.stderr)
        return 2

    if not changes:
        print("no write needed")
        return 0

    if args.dry_run:
        print("dry run; no files written")
        return 0

    backup = backup_file(medley, args.backup_suffix)
    medley.write_text(rewritten, encoding="utf-8", newline="")
    print(f"backup: {backup}")
    print("written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
