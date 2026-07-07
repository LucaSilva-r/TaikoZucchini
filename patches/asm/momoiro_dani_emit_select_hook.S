    .section .rodata.taiko_patch_payload.momoiro_dani_emit_select,"a",@progbits
    .align 2
    .globl taiko_momoiro_dani_emit_gate_hook_start
    .globl taiko_momoiro_dani_emit_gate_hook_end
    .globl taiko_momoiro_dani_select_row_diag_hook_start
    .globl taiko_momoiro_dani_select_row_diag_hook_end
    .type taiko_momoiro_dani_emit_gate_hook_start, @object
    .type taiko_momoiro_dani_select_row_diag_hook_start, @object

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
    .set r31,31
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_EQ,30
    .set CR7_LT,28

    .macro STORE_HEX32 src, off
    mr      r7,\src
    addi    r8,r1,\off
    li      r9,8
1:
    srwi    r10,r7,28
    cmpwi   cr7,r10,10
    addi    r10,r10,48
    bc      BO_IF_TRUE,CR7_LT,2f
    addi    r10,r10,39
2:
    stb     r10,0(r8)
    slwi    r7,r7,4
    addi    r8,r8,1
    addi    r9,r9,-1
    cmpwi   cr7,r9,0
    bc      BO_IF_FALSE,CR7_EQ,1b
    .endm

taiko_momoiro_dani_emit_gate_hook_start:
    /*
     * Hooked over Momoiro 0x00528540:
     *   beq cr7, loc_5285D0
     *
     * The shared pre-RED hook still leaves Momoiro taking the generic
     * Song Select transition. For Momoiro, let native type-9 rows fall into
     * the existing jump-table case at 0x005293F8, which emits row type 0x0C.
     */
    cmpwi   cr7,r0,9
    bc      BO_IF_FALSE,CR7_EQ,momoiro_emit_return

    stdu    r1,-192(r1)
    std     r0,176(r1)
    std     r2,168(r1)
    std     r3,160(r1)
    std     r4,152(r1)
    std     r5,144(r1)
    std     r6,136(r1)
    std     r7,128(r1)
    std     r8,120(r1)
    std     r9,112(r1)
    std     r10,104(r1)
    std     r11,96(r1)
    mfcr    r12
    std     r12,88(r1)

    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " me " */
    ori     r12,r12,0x6520
    stw     r12,36(r1)
    lis     r12,0x7479      /* "type" */
    ori     r12,r12,0x7065
    stw     r12,40(r1)
    lis     r12,0x390A      /* "9\n" */
    stw     r12,44(r1)
    li      r12,0
    stw     r12,72(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,14
    addi    r6,r1,72
    li      r11,0x193
    sc      /* sys_tty_write */

    ld      r12,88(r1)
    mtcrf   0xff,r12
    ld      r11,96(r1)
    ld      r10,104(r1)
    ld      r9,112(r1)
    ld      r8,120(r1)
    ld      r7,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,176(r1)
    addi    r1,r1,192

momoiro_emit_return:
    lis     r12,0x0052
    ori     r12,r12,0x8544
    mtctr   r12
    bctr

taiko_momoiro_dani_emit_gate_hook_end:
    .size taiko_momoiro_dani_emit_gate_hook_start, . - taiko_momoiro_dani_emit_gate_hook_start

taiko_momoiro_dani_select_row_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B65B4:
     *   lwz r31,0(r9)
     *
     * Log the row type consumed by the native top-level transition dispatcher.
     * A correct Dani row must report 0x0000000C before the JINGLE_DANI path.
     */
    lwz     r31,0(r9)

    stdu    r1,-192(r1)
    std     r0,176(r1)
    std     r2,168(r1)
    std     r3,160(r1)
    std     r4,152(r1)
    std     r5,144(r1)
    std     r6,136(r1)
    std     r7,128(r1)
    std     r8,120(r1)
    std     r9,112(r1)
    std     r10,104(r1)
    std     r11,96(r1)
    std     r31,88(r1)
    mfcr    r12
    std     r12,80(r1)

    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " ms " */
    ori     r12,r12,0x7320
    stw     r12,36(r1)
    lis     r12,0x726F      /* "row=" */
    ori     r12,r12,0x773D
    stw     r12,40(r1)
    lis     r12,0x3078      /* "0x" */
    stw     r12,44(r1)
    STORE_HEX32 r31,46
    li      r12,10
    stb     r12,54(r1)
    li      r12,0
    stw     r12,64(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,23
    addi    r6,r1,64
    li      r11,0x193
    sc      /* sys_tty_write */

    ld      r12,80(r1)
    mtcrf   0xff,r12
    ld      r31,88(r1)
    ld      r11,96(r1)
    ld      r10,104(r1)
    ld      r9,112(r1)
    ld      r8,120(r1)
    ld      r7,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,176(r1)
    addi    r1,r1,192

    lis     r12,0x000B
    ori     r12,r12,0x65B8
    mtctr   r12
    bctr

taiko_momoiro_dani_select_row_diag_hook_end:
    .size taiko_momoiro_dani_select_row_diag_hook_start, . - taiko_momoiro_dani_select_row_diag_hook_start
