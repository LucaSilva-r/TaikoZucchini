    .section .rodata.taiko_patch_payload.murasaki_dani_taikojuku,"a",@progbits
    .align 2
    .globl taiko_murasaki_dani_taikojuku_hook_start
    .globl taiko_murasaki_dani_taikojuku_hook_end
    .type taiko_murasaki_dani_taikojuku_hook_start, @object

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
    .set r13,13
    .set r14,14
    .set r15,15
    .set r16,16
    .set r17,17
    .set r18,18
    .set r19,19
    .set r20,20
    .set r21,21
    .set r22,22
    .set r23,23
    .set r24,24
    .set r25,25
    .set r26,26
    .set r27,27
    .set r28,28
    .set r29,29
    .set r30,30
    .set r31,31
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set BO_DEC_NOT_ZERO,16
    .set CR7_LT,28
    .set CR7_GT,29
    .set CR7_EQ,30

taiko_murasaki_dani_taikojuku_hook_start:
    /* Murasaki Dani/Taikojuku row hook payload from imported_patch.yml. */
    mflr    r0
    stdu    r1,-384(r1)
    std     r0,368(r1)
    std     r2,360(r1)
    li      r0,0
    stw     r0,64(r1)
    stw     r0,68(r1)
    stw     r0,72(r1)

    /* Preserve original Dani visibility: row 0x0D only appears on song 1. */
    lwz     r11,0(r25)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_skip_dani_row
    clrldi  r11,r11,32
    lwz     r9,28(r11)
    cmpwi   cr7,r9,1
    bc      BO_IF_FALSE,CR7_EQ,murasaki_skip_dani_row
    li      r0,13
    stw     r0,256(r1)
    lwz     r0,16(r30)
    stw     r0,260(r1)
    lwz     r0,20(r30)
    stw     r0,264(r1)
    li      r0,-1
    stw     r0,268(r1)
    clrldi  r3,r22,32
    lwz     r4,8(r3)
    li      r5,1
    addi    r6,r1,256
    lis     r12,0x005D
    ori     r12,r12,0x6844
    mtctr   r12
    bctrl
    ld      r2,360(r1)

murasaki_skip_dani_row:
    clrldi  r3,r22,32
    lwz     r4,4(r3)
    cmpwi   cr7,r4,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0094
    clrldi  r4,r4,32
    lwz     r6,8(r3)
murasaki_hook_007c:
    cmplw   cr7,r4,r6
    bc      BO_IF_FALSE,CR7_LT,murasaki_hook_0098
    lwz     r8,0(r4)
    cmpwi   cr7,r8,17
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_036c
    addi    r4,r4,16
murasaki_hook_0094:
    b       -24 /* murasaki_hook_007c */
murasaki_hook_0098:
    lis     r3,0x00D4
    ori     r3,r3,0x4A18
    nop
    nop
    nop
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_036c
    clrldi  r3,r3,32
    lwz     r0,0(r3)
    cmpwi   cr7,r0,2
    bc      BO_IF_FALSE,CR7_EQ,murasaki_hook_036c
    li      r3,24
    lis     r12,0x007B
    ori     r12,r12,0xD4D8
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_036c
    mr      r9,r3
    li      r0,0
    stw     r0,0(r9)
    stw     r0,4(r9)
    stw     r0,8(r9)
    li      r0,1
    stb     r0,20(r9)
    stb     r0,21(r9)
    li      r0,0
    stw     r0,64(r1)
    stw     r3,68(r1)
    stw     r0,72(r1)
    clrldi  r9,r3,32
    stw     r3,0(r9)
    stw     r3,4(r9)
    stw     r3,8(r9)
    li      r10,1
    lwz     r11,8(r25)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0180
    clrldi  r11,r11,32
    lbz     r0,0(r11)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_015c
    lwz     r9,40(r11)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_015c
    clrldi  r9,r9,32
    lwz     r10,708(r9)
    b       . + (murasaki_hook_0180 - .)
murasaki_hook_015c:
    addi    r11,r11,44
    lbz     r0,0(r11)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0180
    lwz     r9,40(r11)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0180
    clrldi  r9,r9,32
    lwz     r10,708(r9)
murasaki_hook_0180:
    cmpwi   cr7,r10,1
    bc      BO_IF_TRUE,CR7_LT,murasaki_hook_0190
    cmpwi   cr7,r10,10
    bc      BO_IF_FALSE,CR7_GT,murasaki_hook_0194
murasaki_hook_0190:
    li      r10,1
murasaki_hook_0194:
    mr      r4,r10
    addi    r3,r1,16
    lis     r12,0x0008
    ori     r12,r12,0xFBF4
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    lwz     r8,16(r1)
    lwz     r9,20(r1)
    cmplw   cr7,r8,r9
    bc      BO_IF_FALSE,CR7_LT,murasaki_hook_0348
    stw     r8,24(r1)
    subf    r9,r8,r9
    srawi   r9,r9,3
    cmpwi   cr7,r9,0
    bc      BO_IF_FALSE,CR7_GT,murasaki_hook_0348
    cmpwi   cr7,r9,3
    bc      BO_IF_FALSE,CR7_GT,murasaki_hook_01e0
    li      r9,3
murasaki_hook_01e0:
    stw     r9,28(r1)
    li      r0,0
    stw     r0,48(r1)
    addi    r6,r1,128
    stw     r6,52(r1)
    rlwinm  r7,r9,3,0,28
    add     r7,r6,r7
    stw     r7,56(r1)
    stw     r7,60(r1)
    mtctr   r9
    li      r5,0
murasaki_hook_020c:
    lwz     r8,24(r1)
    clrldi  r8,r8,32
    lwz     r0,0(r8)
    stw     r0,0(r6)
    stw     r5,4(r6)
    addi    r8,r8,8
    stw     r8,24(r1)
    addi    r6,r6,8
    bc      BO_DEC_NOT_ZERO,0,murasaki_hook_020c
    addi    r3,r1,64
    addi    r4,r1,48
    lis     r12,0x0008
    ori     r12,r12,0x7E80
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    addi    r11,r1,64
    stw     r11,112(r1)
    addi    r3,r1,112
    clrldi  r4,r29,32
    lis     r12,0x0008
    ori     r12,r12,0x86A0
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    lwz     r9,0(r25)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0348
    clrldi  r10,r9,32
    lwz     r11,4(r10)
    li      r8,0
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_029c
    lwz     r0,8(r10)
    subf    r8,r11,r0
    srawi   r8,r8,2
murasaki_hook_029c:
    lwz     r11,4(r29)
    li      r6,0
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_02c4
    lwz     r0,8(r29)
    subf    r0,r11,r0
    srawi   r0,r0,2
    lis     r12,0x4F72
    ori     r12,r12,0xC235
    mullw   r6,r0,r12
murasaki_hook_02c4:
    li      r0,0
    stw     r0,80(r1)
    stw     r8,84(r1)
    li      r0,-1
    stw     r0,88(r1)
    stw     r9,92(r1)
    stw     r6,96(r1)
    addi    r3,r1,80
    addi    r4,r1,48
    lis     r12,0x005D
    ori     r12,r12,0x60F0
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    lwz     r6,80(r1)
    cmpwi   cr7,r6,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_0348
    li      r0,17
    stw     r0,256(r1)
    lwz     r0,84(r1)
    stw     r0,260(r1)
    stw     r6,264(r1)
    li      r0,-1
    stw     r0,268(r1)
    clrldi  r3,r22,32
    lwz     r4,8(r3)
    li      r5,1
    addi    r6,r1,256
    lis     r12,0x005D
    ori     r12,r12,0x6844
    mtctr   r12
    bctrl
    ld      r2,360(r1)
murasaki_hook_0348:
    lwz     r3,68(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,murasaki_hook_036c
    addi    r3,r1,64
    lis     r12,0x005C
    ori     r12,r12,0xE64C
    mtctr   r12
    bctrl
    ld      r2,360(r1)
murasaki_hook_036c:
    lwz     r7,4(r27)
    ld      r0,368(r1)
    ld      r2,360(r1)
    mtlr    r0
    addi    r1,r1,384
    lis     r12,0x005D
    ori     r12,r12,0x7BF8
    mtctr   r12
    bctr

taiko_murasaki_dani_taikojuku_hook_end:
    .size taiko_murasaki_dani_taikojuku_hook_start, . - taiko_murasaki_dani_taikojuku_hook_start
