    .section .rodata.taiko_patch_payload.kimidori_dani_dojo,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_dojo_hook_start
    .globl taiko_kimidori_dani_dojo_hook_end
    .type taiko_kimidori_dani_dojo_hook_start, @object

    .set r0,0
    .set r1,1
    .set r2,2
    .set r3,3
    .set r4,4
    .set r5,5
    .set r6,6
    .set r7,7
    .set r8,8
    .set r9,9
    .set r10,10
    .set r11,11
    .set r12,12
    .set r22,22
    .set r24,24
    .set r26,26
    .set r27,27
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_EQ,30

taiko_kimidori_dani_dojo_hook_start:
    /*
     * Hooked at Kimidori 0x57C588, the dormant type-9 Dani row case.
     * Mirror the Murasaki/White row path: append row 0x0D using the original
     * source row fields from r27 + 0x10 / +0x14, then continue after the
     * game's normal row append block. Taikojuku is intentionally not ported.
     */
    stdu    r1,-192(r1)
    std     r0,176(r1)
    mflr    r0
    std     r0,184(r1)
    std     r2,168(r1)
    std     r3,160(r1)
    std     r4,152(r1)
    std     r5,144(r1)
    std     r6,136(r1)
    std     r8,128(r1)
    std     r9,120(r1)
    std     r10,112(r1)
    std     r11,104(r1)
    std     r12,96(r1)

    /* Preserve original Dani visibility: row 0x0D only appears on song 1. */
    lwz     r11,0(r24)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_skip_dani_row
    clrldi  r11,r11,32
    lwz     r9,0x18(r11)
    cmpwi   cr7,r9,1
    bc      BO_IF_FALSE,CR7_EQ,kimidori_skip_dani_row

    li      r0,13
    stw     r0,32(r1)
    lwz     r0,0x10(r27)
    stw     r0,36(r1)
    lwz     r0,0x14(r27)
    stw     r0,40(r1)
    li      r0,-1
    stw     r0,44(r1)

    clrldi  r3,r22,32
    lwz     r4,8(r3)
    li      r5,1
    addi    r6,r1,32
    lis     r12,0x0057
    ori     r12,r12,0x8214
    mtctr   r12
    bctrl
    ld      r2,168(r1)

kimidori_skip_dani_row:
    ld      r12,96(r1)
    ld      r11,104(r1)
    ld      r10,112(r1)
    ld      r9,120(r1)
    ld      r8,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,184(r1)
    mtlr    r0
    ld      r0,176(r1)
    addi    r1,r1,192

    /*
     * The original append path reaches 0x57BC88 with r7 refreshed from the
     * row-tree cursor root at r26 + 4.
     */
    lwz     r7,4(r26)
    lis     r12,0x0057
    ori     r12,r12,0xBC88
    mtctr   r12
    bctr

taiko_kimidori_dani_dojo_hook_end:
    .size taiko_kimidori_dani_dojo_hook_start, . - taiko_kimidori_dani_dojo_hook_start
