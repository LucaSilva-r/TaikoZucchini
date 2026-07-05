    .section .rodata.taiko_patch_payload.kimidori_dani_proc_main,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_proc_main_hook_start
    .globl taiko_kimidori_dani_proc_main_hook_end
    .type taiko_kimidori_dani_proc_main_hook_start, @object

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
    .set CR7_EQ,30
    .set CR7_LT,28

taiko_kimidori_dani_proc_main_hook_start:
    /*
     * Hooked over Kimidori 0x005666C:
     *   cmpwi cr7,r0,0x1A
     *
     * Preserve Kimidori's native marker 0x1A and route it to the existing
     * Dani branch target. Every other marker resumes the original non-Dani
     * path at 0x0056674.
     */
    stdu    r1,-192(r1)
    std     r0,176(r1)
    std     r3,168(r1)
    std     r4,160(r1)
    std     r5,152(r1)
    std     r6,144(r1)
    std     r7,136(r1)
    std     r8,128(r1)
    std     r9,120(r1)
    std     r10,112(r1)
    std     r11,104(r1)
    std     r12,96(r1)
    mfcr    r12
    std     r12,88(r1)

    /* Emit one runtime marker line: "[tz] pm=0xNN\n". */
    lis     r12,0x5B74  /* 0x5B747A5D */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2070  /* 0x20706D3D */
    ori     r12,r12,0x6D3D
    stw     r12,36(r1)
    lis     r12,0x3078
    stw     r12,40(r1)
    li      r12,10
    stb     r12,44(r1)
    li      r12,0
    stw     r12,48(r1)

    ld      r12,176(r1)
    rlwinm  r11,r12,28,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_proc_main_hi_store
    addi    r11,r11,39
kimidori_proc_main_hi_store:
    stb     r11,42(r1)

    ld      r12,176(r1)
    rlwinm  r11,r12,0,28,31
    cmpwi   cr7,r11,10
    addi    r11,r11,48
    bc      BO_IF_TRUE,CR7_LT,kimidori_proc_main_lo_store
    addi    r11,r11,39
kimidori_proc_main_lo_store:
    stb     r11,43(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,13
    addi    r6,r1,48
    li      r11,0x193
    sc      /* sys_tty_write */

    ld      r12,88(r1)
    mtcrf   0xff,r12
    ld      r12,96(r1)
    ld      r11,104(r1)
    ld      r10,112(r1)
    ld      r9,120(r1)
    ld      r8,128(r1)
    ld      r7,136(r1)
    ld      r6,144(r1)
    ld      r5,152(r1)
    ld      r4,160(r1)
    ld      r3,168(r1)
    ld      r0,176(r1)
    addi    r1,r1,192

    cmpwi   cr7,r0,0x1A
    bc      BO_IF_TRUE,CR7_EQ,kimidori_proc_main_dani

    lis     r12,0x0005
    ori     r12,r12,0x6674
    mtctr   r12
    bctr

kimidori_proc_main_dani:
    lis     r12,0x0005
    ori     r12,r12,0x66E8
    mtctr   r12
    bctr

taiko_kimidori_dani_proc_main_hook_end:
    .size taiko_kimidori_dani_proc_main_hook_start, . - taiko_kimidori_dani_proc_main_hook_start
