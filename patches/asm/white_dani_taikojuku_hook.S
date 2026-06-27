    .section .rodata.taiko_patch_payload.white_dani_taikojuku,"a",@progbits
    .align 2
    .globl taiko_white_dani_taikojuku_hook_start
    .globl taiko_white_dani_taikojuku_hook_end
    .type taiko_white_dani_taikojuku_hook_start, @object

taiko_white_dani_taikojuku_hook_start:
    /* Prologue and original White 0x0D row append. */
    .long 0x7C0802A6
    .long 0xF821FE81
    .long 0xF8010170
    .long 0xF8410168
    .long 0x386101F8
    .long 0x3880000D
    .long 0x80BD0010
    .long 0x80DD0014
    .long 0x38E0FFFF
    .long 0x3D800067
    .long 0x618C9DC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Guard against duplicate visible row 0x11 in the local row vector. */
    .long 0x38000000
    .long 0x90010040
    .long 0x90010044
    .long 0x90010048
    .long 0x386101F8
    .long 0x80830000
    .long 0x2F840000
    .long 0x419E02F8
    .long 0x78840020
    .long 0x80A40004
    .long 0x80C40008
    .long 0x7F853040
    .long 0x409C0020
    .long 0x78A70020
    .long 0x81070000
    .long 0x2F880011
    .long 0x419E02D4
    .long 0x38A50010
    .long 0x7F853040
    .long 0x419CFFE8

    /* Resolve and validate the White selected-pack object. */
    .long 0x3D800009
    .long 0x618C542C
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x2F830000
    .long 0x419E02AC
    .long 0x78630020
    .long 0x80030000
    .long 0x2F800002
    .long 0x409E029C
    .long 0x38600018
    .long 0x3D800085
    .long 0x618CFCC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x2F830000
    .long 0x419E027C

    /* Initialize raw Taikojuku row state. */
    .long 0x7C691B78
    .long 0x38000000
    .long 0x90090000
    .long 0x90090004
    .long 0x90090008
    .long 0x38000001
    .long 0x98090014
    .long 0x98090015
    .long 0x38000000
    .long 0x90010040
    .long 0x90610044
    .long 0x90010048
    .long 0x78690020
    .long 0x90690000
    .long 0x90690004
    .long 0x90690008

    /* Load selected raw pack number from White's +0x2D0 fields. */
    .long 0x39400001
    .long 0x81750008
    .long 0x2F8B0000
    .long 0x419E0050
    .long 0x796B0020
    .long 0x880B0000
    .long 0x2F800000
    .long 0x419E001C
    .long 0x812B0028
    .long 0x2F890000
    .long 0x419E0010
    .long 0x79290020
    .long 0x814902D0
    .long 0x48000028
    .long 0x396B002C
    .long 0x880B0000
    .long 0x2F800000
    .long 0x419E0018
    .long 0x812B0028
    .long 0x2F890000
    .long 0x419E000C
    .long 0x79290020
    .long 0x814902D0
    .long 0x2F8A0001
    .long 0x419C000C
    .long 0x2F8A000A
    .long 0x409D0008
    .long 0x39400001

    /* Resolve raw pack through White helper 0x00954550. */
    .long 0x38610010
    .long 0x7D445378
    .long 0x3D800009
    .long 0x618C5450
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Clamp and copy the returned raw music list. */
    .long 0x81010010
    .long 0x81210014
    .long 0x7F884840
    .long 0x409C017C
    .long 0x91010018
    .long 0x7D284850
    .long 0x7D291E70
    .long 0x2F890000
    .long 0x409D0168
    .long 0x2F890003
    .long 0x409D0008
    .long 0x39200003
    .long 0x9121001C
    .long 0x38000000
    .long 0x90010030
    .long 0x38C10080
    .long 0x90C10034
    .long 0x55271838
    .long 0x7CE63A14
    .long 0x90E10038
    .long 0x90E1003C
    .long 0x7D2903A6
    .long 0x38A00000
    .long 0x81010018
    .long 0x79080020
    .long 0x80080000
    .long 0x90060000
    .long 0x90A60004
    .long 0x39080008
    .long 0x91010018
    .long 0x38C60008
    .long 0x4200FFE0

    /* Bind music indices through White helpers. */
    .long 0x38610040
    .long 0x38810030
    .long 0x3D800008
    .long 0x618C7C88
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x39610040
    .long 0x91610070
    .long 0x38610070
    .long 0x389301E8
    .long 0x3D800008
    .long 0x618C6B80
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Compute backing-vector append metadata and extra=-1 semantics. */
    .long 0x81350000
    .long 0x2F890000
    .long 0x419E00C0
    .long 0x792A0020
    .long 0x816A0004
    .long 0x39000000
    .long 0x2F8B0000
    .long 0x419E0010
    .long 0x800A0008
    .long 0x7D0B0050
    .long 0x7D081670
    .long 0x38F301E8
    .long 0x80A70004
    .long 0x38C00000
    .long 0x2F850000
    .long 0x419E001C
    .long 0x80070008
    .long 0x7C050050
    .long 0x7C001670
    .long 0x3D804F72
    .long 0x618CC235
    .long 0x7CC061D6
    .long 0x38000000
    .long 0x90010050
    .long 0x91010054
    .long 0x3800FFFF
    .long 0x90010058
    .long 0x9121005C
    .long 0x90C10060
    .long 0x38610050
    .long 0x38810030
    .long 0x3D800067
    .long 0x618CA2F0
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Append visible row 0x11 and clean row-local state. */
    .long 0x80C10050
    .long 0x2F860000
    .long 0x419E0030
    .long 0x386101F8
    .long 0x38800011
    .long 0x80A10054
    .long 0x7CA507B4
    .long 0x7CC607B4
    .long 0x38E0FFFF
    .long 0x3D800067
    .long 0x618C9DC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x80610044
    .long 0x2F830000
    .long 0x419E001C
    .long 0x38610040
    .long 0x3D800067
    .long 0x618CAD7C
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Epilogue and explicit jump to White row-iteration continuation. */
    .long 0xE8010170
    .long 0xE8410168
    .long 0x7C0803A6
    .long 0x38210180
    .long 0x3D800067
    .long 0x618CDE40
    .long 0x7D8903A6
    .long 0x4E800420

taiko_white_dani_taikojuku_hook_end:
    .size taiko_white_dani_taikojuku_hook_start, . - taiko_white_dani_taikojuku_hook_start
