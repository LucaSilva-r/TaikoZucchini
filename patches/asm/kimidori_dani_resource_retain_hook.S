    .section .rodata.taiko_patch_payload.kimidori_dani_resource_retain,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_resource_retain_hook_start
    .globl taiko_kimidori_dani_resource_retain_hook_end
    .type taiko_kimidori_dani_resource_retain_hook_start, @object

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
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR0_EQ,2
    .set CR7_EQ,30
    .set CR7_LT,28

taiko_kimidori_dani_resource_retain_hook_start:
    /*
     * Hooked over Kimidori 0x003E6F14:
     *   bl sub_3BF608
     *
     * Preserve the registry insertion call, then keep one extra reference for
     * the Dani title-resource family. The range comes from the validated
     * runtime patch that stopped the Dani select crash:
     *   0x000A0000 <= key < 0x000A01A0.
     */
    stdu    r1,-448(r1)
    std     r0,432(r1)
    mflr    r0
    std     r0,424(r1)
    std     r2,416(r1)
    std     r3,408(r1)
    std     r4,400(r1)
    std     r5,392(r1)
    std     r6,384(r1)
    std     r7,376(r1)
    std     r8,368(r1)
    std     r9,360(r1)
    std     r10,352(r1)
    std     r11,344(r1)
    std     r12,336(r1)
    mfcr    r12
    std     r12,328(r1)

    ld      r3,408(r1)
    ld      r4,400(r1)
    ld      r5,392(r1)
    lis     r12,0x003B
    ori     r12,r12,0xF608
    mtctr   r12
    bctrl
    std     r3,312(r1)
    ld      r2,416(r1)

    ld      r11,400(r1)
    srwi    r12,r11,16
    cmpwi   cr7,r12,0x000A
    bc      BO_IF_FALSE,CR7_EQ,kimidori_dani_retain_done

    ld      r11,400(r1)
    lis     r12,0x000A
    ori     r12,r12,0x01A0
    cmplw   cr7,r11,r12
    bc      BO_IF_FALSE,CR7_LT,kimidori_dani_retain_done

    ld      r11,312(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_dani_retain_done

    ld      r11,392(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_dani_retain_done

    addi    r9,r11,4
    clrldi  r9,r9,32
kimidori_dani_retain_loop:
    lwarx   r10,0,r9
    addi    r8,r10,1
    stwcx.  r8,0,r9
    bc      BO_IF_FALSE,CR0_EQ,kimidori_dani_retain_loop

kimidori_dani_retain_done:
    ld      r2,416(r1)
    ld      r3,312(r1)
    addi    r1,r1,448

    lis     r12,0x003E
    ori     r12,r12,0x6F18
    mtctr   r12
    bctr

taiko_kimidori_dani_resource_retain_hook_end:
    .size taiko_kimidori_dani_resource_retain_hook_start, . - taiko_kimidori_dani_resource_retain_hook_start
