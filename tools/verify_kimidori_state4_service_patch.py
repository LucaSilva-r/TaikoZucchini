#!/usr/bin/env python3
"""Static regression checks for the Kimidori Dani state-4 service-table patch."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "eboot_patcher" / "eboot_inline_specs.c"
ASM = ROOT / "patches" / "asm" / "kimidori_dani_runtime_diag_hooks.S"
MAKEFILE = ROOT / "Makefile"
MAKEFILE_WIN = ROOT / "Makefile.win"


def require(condition, message):
    if not condition:
        print("FAIL:", message)
        return False
    return True


def main():
    src = SPEC.read_text(encoding="utf-8")
    asm = ASM.read_text(encoding="utf-8")
    makefile = MAKEFILE.read_text(encoding="utf-8")
    makefile_win = MAKEFILE_WIN.read_text(encoding="utf-8")
    ok = True

    required_tokens = [
        "patch_kimidori_dani_state4_service_table",
        "KIMIDORI_DANI_STATE4_TOC_VA",
        "KIMIDORI_DANI_STATE4_ORIGINAL_TABLE_WORDS",
        "KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS",
        "KIMIDORI_DANI_STATE4_SERVICE_WORDS",
        "KIMIDORI_DANI_LOOKUP_DIAG_SIGNATURES",
        "KIMIDORI_DANI_STATE4_SERVICE_DIAG_SIGNATURES",
        "taiko_kimidori_dani_lookup_diag_hook_start",
        "taiko_kimidori_dani_state4_service_diag_hook_start",
        "kimidori-st51-v05r00-dani-lookup-diag",
        "kimidori-st51-v05r00-dani-state4-service-diag",
        "0x003B2520u",
        "0x00056858u",
    ]
    for token in required_tokens:
        ok &= require(token in src, f"missing {token}")

    asm_tokens = [
        "taiko_kimidori_dani_lookup_diag_hook_start",
        "taiko_kimidori_dani_state4_service_diag_hook_start",
        "0xB70C",
        "0x2524",
        "0x685C",
        "sys_tty_write",
    ]
    for token in asm_tokens:
        ok &= require(token in asm, f"runtime diagnostic asm missing {token}")

    ok &= require(
        "patches/asm/kimidori_dani_runtime_diag_hooks.S" in makefile,
        "GNU Makefile missing runtime diagnostic asm",
    )
    ok &= require(
        "patches\\asm\\kimidori_dani_runtime_diag_hooks.o" in makefile_win,
        "Windows Makefile missing runtime diagnostic object",
    )

    table_match = re.search(
        r"KIMIDORI_DANI_STATE4_SERVICE_TABLE_WORDS\[\]\s*=\s*\{(?P<body>.*?)\};",
        src,
        re.S,
    )
    ok &= require(table_match is not None, "missing state-4 service table body")
    if table_match:
        body = table_match.group("body")
        ordered_words = [
            "0x00056590u",
            "0x00B3C2B8u",
            "0x00056844u",
            "0x00B3C2B8u",
            "0x0049E708u",
            "0x00B3C2B8u",
            "0x004A9430u",
            "0x00B3C2B8u",
            "0x004A8290u",
            "0x00B3C2B8u",
            "0x004C5C64u",
            "0x00B3C2B8u",
        ]
        pos = -1
        for word in ordered_words:
            next_pos = body.find(word, pos + 1)
            ok &= require(next_pos >= 0, f"state-4 table missing ordered word {word}")
            pos = next_pos

    apply_index = src.find("patch_kimidori_dani_state4_service_table(ctx)")
    hooks_index = src.find("eboot_inline_hook_apply(ctx")
    ok &= require(apply_index >= 0, "state-4 service patch is not invoked")
    ok &= require(hooks_index >= 0, "inline hook apply call not found")
    ok &= require(
        0 <= apply_index < hooks_index,
        "state-4 service patch must run before inline hook installation",
    )

    forbidden = ["0x000A00EC", "entry/packeddata", "root slot"]
    for token in forbidden:
        ok &= require(token not in src, f"forbidden resource-graft token present: {token}")

    if not ok:
        return 1
    print("Kimidori state-4 service patch static checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
