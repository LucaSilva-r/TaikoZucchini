#!/usr/bin/env python3
"""Merge ST5100-1 Dani definitions into active Kimidori data.

The Murasaki install carries the Kimidori-era ST5100-1 Dani set with 22 ranks.
This tool uses ST5100-1 for the rank order, medley IDs, and missing rank rows,
while preserving active Kimidori medleyinfo row contents for ranks already
present. The musicinfo.xml Dani medley Data rows are copied from ST5100-1 as
the correct Kimidori-era medley song metadata. musicmedleyinfo output keeps
the target archive format by default; old-format files represent ura oni as
`difficulty=3` plus `hidden=1`, while newer files use `difficulty=4`.
"""

from __future__ import annotations

import argparse
import re
import shutil
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


DATA_RE = re.compile(r"(<Data\b[^>]*>.*?</Data>)", re.DOTALL)
DATA_OPEN_RE = re.compile(r"<Data\b[^>]*>")
MEDLEY_RE = re.compile(
    r"(<MusicMedleyInfoData\b[^>]*>.*?</MusicMedleyInfoData>)",
    re.DOTALL,
)
MUSICID_RE = re.compile(r"<musicid>([^<]+)</musicid>")
UNIQUEID_RE = re.compile(r"(<uniqueid>)(\d+)(</uniqueid>)")
PARTSSET_RE = re.compile(r"<partsset>([^<]+)</partsset>")
CLASS_ID_RE = re.compile(r'(class_id=")(\d+)(")')
HIDDEN_LINE_RE = re.compile(r"^[ \t]*<hidden>[^<]*</hidden>\r?\n?", re.MULTILINE)
CONTENT_RE = re.compile(r"(<Content\b[^>]*>.*?</Content>)", re.DOTALL)
HIDDEN_RE = re.compile(r"(<hidden>)(\d+)(</hidden>)")
NOTES_RE = re.compile(r"(?P<indent>[ \t]*)<notes>")

LEGACY_HIDDEN_VERSION_CUTOFF = 0x20140500


GRADE_DIGITS = {
    "1": "一",
    "１": "一",
    "2": "二",
    "２": "二",
    "3": "三",
    "３": "三",
    "4": "四",
    "４": "四",
    "5": "五",
    "５": "五",
    "6": "六",
    "６": "六",
    "7": "七",
    "７": "七",
    "8": "八",
    "８": "八",
    "9": "九",
    "９": "九",
    "10": "十",
    "１０": "十",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Port the 22-rank ST5100-1 Dani shape into active Kimidori data. "
            "musicmedleyinfo keeps existing Kimidori rank contents where "
            "present; musicinfo medley rows are copied from ST5100-1."
        )
    )
    parser.add_argument(
        "--target-data-dir",
        type=Path,
        required=True,
        help="Active Kimidori data directory containing musicinfo.xml.",
    )
    parser.add_argument(
        "--reference-dir",
        type=Path,
        required=True,
        help="ST5100-1 reference directory containing musicinfo.xml.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report changes without writing backups or files.",
    )
    parser.add_argument(
        "--backup-suffix",
        default=None,
        help="Backup suffix. Default: .bak-YYYYMMDD-HHMMSS.",
    )
    parser.add_argument(
        "--musicmedley-version",
        default=None,
        help=(
            "Override output musicmedleyinfo header version. Default: keep "
            "the target file's version."
        ),
    )
    parser.add_argument(
        "--target-musicmedleyinfo-source",
        type=Path,
        default=None,
        help=(
            "Optional musicmedleyinfo.xml source to read existing Kimidori "
            "rank contents from while still writing to --target-data-dir."
        ),
    )
    return parser.parse_args()


def tag_text(block: str, tag: str) -> str:
    match = re.search(rf"<{tag}>([^<]*)</{tag}>", block)
    return match.group(1).strip() if match else ""


def replace_tag(block: str, tag: str, value: str, count: int = 1) -> str:
    return re.sub(
        rf"(<{tag}>)([^<]*)(</{tag}>)",
        rf"\g<1>{value}\g<3>",
        block,
        count=count,
    )


def replace_first_uniqueid(block: str, value: str) -> str:
    return UNIQUEID_RE.sub(rf"\g<1>{value}\g<3>", block, count=1)


def normalize_rank(rank: str) -> str:
    rank = rank.strip()
    rank = rank.replace(" ", "")
    if rank.endswith("級"):
        head = rank[:-1]
        return GRADE_DIGITS.get(head, head) + "級"
    return rank


def musicinfo_rank(block: str) -> str:
    musicname = tag_text(block, "musicname")
    match = re.search(r"段位道場\(([^)]+)\)", musicname)
    if match:
        return normalize_rank(match.group(1))
    return ""


def is_medley_musicinfo_block(block: str) -> bool:
    musicid = tag_text(block, "musicid")
    if not re.fullmatch(r"medley\d+", musicid):
        return False
    partsset = tag_text(block, "partsset")
    return partsset in ("", "dojo")


def medleyinfo_rank(block: str) -> str:
    return normalize_rank(tag_text(block, "medleyname"))


def split_blocks(text: str, pattern: re.Pattern[str]) -> Tuple[str, List[str], str]:
    matches = list(pattern.finditer(text))
    if not matches:
        return text, [], ""
    prefix = text[: matches[0].start()]
    suffix = text[matches[-1].end() :]
    return prefix, [match.group(1) for match in matches], suffix


def map_by_rank(blocks: Iterable[str], rank_fn) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for block in blocks:
        rank = rank_fn(block)
        if not rank:
            continue
        if rank in result:
            raise SystemExit(f"Duplicate rank in input data: {rank}")
        result[rank] = block
    return result


def reference_rank_order(blocks: Sequence[str], rank_fn) -> List[str]:
    ranks = [rank_fn(block) for block in blocks]
    if any(not rank for rank in ranks):
        raise SystemExit("Reference data has an unrecognized Dani rank")
    if len(set(ranks)) != len(ranks):
        raise SystemExit("Reference data has duplicate Dani ranks")
    return ranks


def medleyinfo_header_version(prefix: str) -> str:
    match = re.search(
        r"<MusicMedleyInfoHeader\b[^>]*>.*?<version>(\d+)</version>",
        prefix,
        re.DOTALL,
    )
    if match is None:
        raise SystemExit("Could not find MusicMedleyInfoHeader version")
    return match.group(1)


def medleyinfo_uses_hidden(version: str) -> bool:
    return int(version) <= LEGACY_HIDDEN_VERSION_CUTOFF


def validate_medleyinfo_version(version: str) -> str:
    if not re.fullmatch(r"\d+", version):
        raise SystemExit(f"Invalid musicmedleyinfo version: {version}")
    return version


def update_medleyinfo_header(prefix: str, version: str, count: int) -> str:
    rewritten, changed = re.subn(
        r"(<MusicMedleyInfoHeader\b[^>]*>.*?<version>)(\d+)(</version>)",
        rf"\g<1>{version}\g<3>",
        prefix,
        count=1,
        flags=re.DOTALL,
    )
    if changed != 1:
        raise SystemExit("Could not update MusicMedleyInfoHeader version")

    rewritten, changed = re.subn(
        r"(<MusicMedleyInfoHeader\b[^>]*>.*?<size>)(\d+)(</size>.*?</MusicMedleyInfoHeader>)",
        rf"\g<1>{count}\g<3>",
        rewritten,
        count=1,
        flags=re.DOTALL,
    )
    if changed != 1:
        raise SystemExit("Could not update MusicMedleyInfoHeader size")
    return rewritten


def update_musicinfo_header_size(text: str, count: int) -> str:
    pattern = re.compile(
        r"(<Header\b[^>]*>.*?<signature>TaikoAC15 MusicInfo</signature>.*?<size>)(\d+)(</size>)",
        re.DOTALL,
    )
    rewritten, changed = pattern.subn(rf"\g<1>{count}\g<3>", text, count=1)
    if changed != 1:
        raise SystemExit("Could not update MusicInfo Header size")
    return rewritten


def resequence_musicinfo_data_class_ids(text: str) -> str:
    data_matches = list(DATA_RE.finditer(text))
    if not data_matches:
        raise SystemExit("musicinfo.xml has no Data blocks")

    first_open = DATA_OPEN_RE.search(data_matches[0].group(1))
    if first_open is None:
        raise SystemExit("First Data block has no opening tag")
    first_class_id = CLASS_ID_RE.search(first_open.group(0))
    if first_class_id is None:
        raise SystemExit("First Data block has no class_id")
    next_class_id = int(first_class_id.group(2))

    cursor = 0
    parts: List[str] = []
    for match in data_matches:
        block = match.group(1)
        open_match = DATA_OPEN_RE.search(block)
        if open_match is None:
            raise SystemExit("Data block has no opening tag")
        open_tag = open_match.group(0)
        if CLASS_ID_RE.search(open_tag) is None:
            raise SystemExit("Data block has no class_id")

        new_open_tag, changed = CLASS_ID_RE.subn(
            rf"\g<1>{next_class_id}\g<3>",
            open_tag,
            count=1,
        )
        if changed != 1:
            raise SystemExit("Could not rewrite Data class_id")

        new_block = (
            block[: open_match.start()]
            + new_open_tag
            + block[open_match.end() :]
        )
        parts.append(text[cursor : match.start()])
        parts.append(new_block)
        cursor = match.end()
        next_class_id += 1
    parts.append(text[cursor:])
    return "".join(parts)


def content_musicids(block: str) -> List[str]:
    return MUSICID_RE.findall(block)


def convert_content_to_medley_version(content: str, output_uses_hidden: bool) -> str:
    difficulty = tag_text(content, "difficulty")
    hidden = tag_text(content, "hidden")

    if output_uses_hidden:
        if difficulty == "4":
            content = replace_tag(content, "difficulty", "3")
            hidden = "1"
        elif hidden == "":
            hidden = "0"

        if HIDDEN_RE.search(content):
            return HIDDEN_RE.sub(rf"\g<1>{hidden}\g<3>", content, count=1)

        notes_match = NOTES_RE.search(content)
        if notes_match is None:
            raise SystemExit("Content block has no notes tag")
        indent = notes_match.group("indent")
        hidden_line = f"{indent}<hidden>{hidden}</hidden>\n"
        return (
            content[: notes_match.start()]
            + hidden_line
            + content[notes_match.start() :]
        )

    if hidden == "1" and difficulty == "3":
        content = replace_tag(content, "difficulty", "4")
    return HIDDEN_LINE_RE.sub("", content)


def convert_contents_to_medley_version(block: str, version: str) -> str:
    output_uses_hidden = medleyinfo_uses_hidden(version)

    def convert(match: re.Match[str]) -> str:
        return convert_content_to_medley_version(match.group(1), output_uses_hidden)

    return CONTENT_RE.sub(convert, block)


def rewrite_medleyinfo(
    target_text: str,
    reference_text: str,
    version_override: str | None = None,
) -> Tuple[str, List[str], List[str]]:
    target_prefix, target_blocks, target_suffix = split_blocks(target_text, MEDLEY_RE)
    ref_prefix, ref_blocks, _ref_suffix = split_blocks(reference_text, MEDLEY_RE)
    if not target_blocks or not ref_blocks:
        raise SystemExit("Missing MusicMedleyInfoData blocks")

    target_by_rank = map_by_rank(target_blocks, medleyinfo_rank)
    ref_order = reference_rank_order(ref_blocks, medleyinfo_rank)
    target_version = medleyinfo_header_version(target_prefix)
    medleyinfo_header_version(ref_prefix)
    output_version = validate_medleyinfo_version(version_override or target_version)

    merged: List[str] = []
    preserved: List[str] = []
    inserted: List[str] = []
    for ref_block in ref_blocks:
        rank = medleyinfo_rank(ref_block)
        ref_uid = tag_text(ref_block, "uniqueid")
        ref_level = tag_text(ref_block, "challengelv")
        ref_name = tag_text(ref_block, "medleyname")

        if rank in target_by_rank:
            block = target_by_rank[rank]
            block = replace_first_uniqueid(block, ref_uid)
            block = replace_tag(block, "medleyname", ref_name)
            block = replace_tag(block, "challengelv", ref_level)
            preserved.append(rank)
        else:
            block = ref_block
            inserted.append(rank)
        block = convert_contents_to_medley_version(block, output_version)
        merged.append(block)

    target_prefix = update_medleyinfo_header(target_prefix, output_version, len(ref_order))
    return target_prefix + "\n  ".join(merged) + target_suffix, preserved, inserted


def rewrite_musicinfo(
    target_text: str,
    reference_text: str,
) -> Tuple[str, int, int]:
    ref_blocks = [
        block for block in DATA_RE.findall(reference_text) if is_medley_musicinfo_block(block)
    ]
    if not ref_blocks:
        raise SystemExit("Reference musicinfo.xml has no medley Data blocks")

    target_matches = [
        match
        for match in DATA_RE.finditer(target_text)
        if is_medley_musicinfo_block(match.group(1))
    ]
    if not target_matches:
        raise SystemExit("Could not find target medley Data insertion point")

    start = target_matches[0].start()
    end = target_matches[-1].end()
    merged = "\n\t\t".join(ref_blocks)
    rewritten = target_text[:start] + merged + target_text[end:]
    rewritten = update_musicinfo_header_size(
        rewritten,
        len(DATA_RE.findall(rewritten)),
    )
    rewritten = resequence_musicinfo_data_class_ids(rewritten)
    return rewritten, len(target_matches), len(ref_blocks)


def backup_file(path: Path, suffix: str | None) -> Path:
    if suffix is None:
        suffix = ".bak-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = path.with_name(path.name + suffix)
    if backup.exists():
        raise SystemExit(f"Backup already exists: {backup}")
    shutil.copy2(path, backup)
    return backup


def print_list(label: str, values: Sequence[str]) -> None:
    print(f"{label}: {len(values)}")
    if values:
        print("  " + ", ".join(values))


def main() -> int:
    args = parse_args()
    target_musicinfo = args.target_data_dir / "musicinfo.xml"
    target_medley = args.target_data_dir / "musicmedleyinfo.xml"
    ref_musicinfo = args.reference_dir / "musicinfo.xml"
    ref_medley = args.reference_dir / "musicmedleyinfo.xml"
    target_medley_source = args.target_musicmedleyinfo_source or target_medley
    for path in (target_musicinfo, target_medley, ref_musicinfo, ref_medley):
        if not path.is_file():
            raise SystemExit(f"Missing required file: {path}")
    if not target_medley_source.is_file():
        raise SystemExit(f"Missing target medley source: {target_medley_source}")

    target_musicinfo_text = target_musicinfo.read_text(encoding="utf-8")
    target_medley_current_text = target_medley.read_text(encoding="utf-8")
    target_medley_text = target_medley_source.read_text(encoding="utf-8")
    ref_musicinfo_text = ref_musicinfo.read_text(encoding="utf-8")
    ref_medley_text = ref_medley.read_text(encoding="utf-8")

    new_medley, medley_preserved, medley_inserted = rewrite_medleyinfo(
        target_medley_text,
        ref_medley_text,
        args.musicmedley_version,
    )
    new_musicinfo, musicinfo_removed, musicinfo_inserted = rewrite_musicinfo(
        target_musicinfo_text,
        ref_musicinfo_text,
    )

    print(f"target: {args.target_data_dir}")
    print(f"reference: {args.reference_dir}")
    if target_medley_source != target_medley:
        print(f"musicmedley source: {target_medley_source}")
    print(f"musicmedley output version: {medleyinfo_header_version(new_medley)}")
    print_list("musicmedley preserved Kimidori ranks", medley_preserved)
    print_list("musicmedley inserted ST5100-1 ranks", medley_inserted)
    print(f"musicinfo removed target medley rows: {musicinfo_removed}")
    print(f"musicinfo inserted ST5100-1 medley rows: {musicinfo_inserted}")
    print(f"musicmedley changed: {new_medley != target_medley_current_text}")
    print(f"musicinfo changed: {new_musicinfo != target_musicinfo_text}")

    if args.dry_run:
        print("dry run; no files written")
        return 0

    backups: List[Path] = []
    if new_musicinfo != target_musicinfo_text:
        backups.append(backup_file(target_musicinfo, args.backup_suffix))
        target_musicinfo.write_text(new_musicinfo, encoding="utf-8", newline="")
    if new_medley != target_medley_current_text:
        backups.append(backup_file(target_medley, args.backup_suffix))
        target_medley.write_text(new_medley, encoding="utf-8", newline="")

    for backup in backups:
        print(f"backup: {backup}")
    print("written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
