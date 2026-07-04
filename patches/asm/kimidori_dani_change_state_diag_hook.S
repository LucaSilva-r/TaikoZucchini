    .section .rodata.taiko_patch_payload.kimidori_dani_change_state_diag,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_change_state_diag_hook_start
    .globl taiko_kimidori_dani_change_state_diag_hook_end
    .type taiko_kimidori_dani_change_state_diag_hook_start, @object

    .set r0,0
    .set r1,1
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
    .set BO_IF_TRUE,12
    .set CR7_LT,28

taiko_kimidori_dani_change_state_diag_hook_start:
    /*
     * Hooked over Kimidori 0x0056430:
     *   cmplwi cr7,r4,9
     *
     * Log GameDojoSelect::ChangeState(this, state) without changing behavior.
     * cs is requested state, ps is this+0x10 before the change, and f is
     * this+0x14, the native Dani flag set before state 7.
     */
    stdu    r1,-224(r1)
    std     r0,208(r1)
    std     r3,200(r1)
    std     r4,192(r1)
    std     r5,184(r1)
    std     r6,176(r1)
    std     r7,168(r1)
    std     r8,160(r1)
    std     r9,152(r1)
    std     r10,144(r1)
    std     r11,136(r1)
    std     r12,128(r1)
    mfcr    r12
    std     r12,120(r1)

    /* Emit one runtime marker line: "[tz] cs=0xNN ps=0xNN f=0xNN\n". */
    lis     r12,0x5B74  /* 0x5B747A5D */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2063  /* 0x2063733D */
    ori     r12,r12,0x733D
    stw     r12,36(r1)
    lis     r12,0x3078
    stw     r12,40(r1)
    lis     r12,0x2070  /* 0x2070733D */
    ori     r12,r12,0x733D
    stw     r12,44(r1)
    lis     r12,0x3078
    stw     r12,48(r1)
    lis     r12,0x2066  /* 0x20663D30 */
    ori     r12,r12,0x3D30
    stw     r12,52(r1)
    lis     r12,0x7800  /* 0x7800000A */
    ori     r12,r12,0x000A
    stw     r12,56(r1)
    li      r12,0
    stw     r12,64(r1)

    ld      r12,192(r1)
    rlwinm  r11,r12,28,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_cs_hi_store
    addi    r11,r11,39
kimidori_change_state_cs_hi_store:
    stb     r11,42(r1)

    ld      r12,192(r1)
    rlwinm  r11,r12,0,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_cs_lo_store
    addi    r11,r11,39
kimidori_change_state_cs_lo_store:
    stb     r11,43(r1)

    ld      r12,200(r1)
    lwz     r12,16(r12)
    rlwinm  r11,r12,28,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_ps_hi_store
    addi    r11,r11,39
kimidori_change_state_ps_hi_store:
    stb     r11,50(r1)

    ld      r12,200(r1)
    lwz     r12,16(r12)
    rlwinm  r11,r12,0,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_ps_lo_store
    addi    r11,r11,39
kimidori_change_state_ps_lo_store:
    stb     r11,51(r1)

    ld      r12,200(r1)
    lwz     r12,20(r12)
    rlwinm  r11,r12,28,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_f_hi_store
    addi    r11,r11,39
kimidori_change_state_f_hi_store:
    stb     r11,57(r1)

    ld      r12,200(r1)
    lwz     r12,20(r12)
    rlwinm  r11,r12,0,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_change_state_f_lo_store
    addi    r11,r11,39
kimidori_change_state_f_lo_store:
    stb     r11,58(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,28
    addi    r6,r1,64
    li      r11,0x193
    sc      /* sys_tty_write */

    ld      r12,120(r1)
    mtcrf   0xff,r12
    ld      r12,128(r1)
    ld      r11,136(r1)
    ld      r10,144(r1)
    ld      r9,152(r1)
    ld      r8,160(r1)
    ld      r7,168(r1)
    ld      r6,176(r1)
    ld      r5,184(r1)
    ld      r4,192(r1)
    ld      r3,200(r1)
    ld      r0,208(r1)
    addi    r1,r1,224

    cmplwi  cr7,r4,9
    lis     r12,0x0005
    ori     r12,r12,0x6434
    mtctr   r12
    bctr

taiko_kimidori_dani_change_state_diag_hook_end:
    .size taiko_kimidori_dani_change_state_diag_hook_start, . - taiko_kimidori_dani_change_state_diag_hook_start
