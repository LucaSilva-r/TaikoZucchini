    .section .rodata.taiko_patch_payload.kimidori_dani_runtime_diag,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_lookup_diag_hook_start
    .globl taiko_kimidori_dani_lookup_diag_hook_end
    .globl taiko_kimidori_dani_state4_service_diag_hook_start
    .globl taiko_kimidori_dani_state4_service_diag_hook_end
    .type taiko_kimidori_dani_lookup_diag_hook_start, @object
    .type taiko_kimidori_dani_state4_service_diag_hook_start, @object

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

taiko_kimidori_dani_lookup_diag_hook_start:
    /*
     * Hooked over Kimidori 0x003B2520:
     *   bl sub_3BB70C
     *
     * Preserve the original lookup call, then log the key, output pointer, and
     * boolean return when the Dani crash key 0xA00EC or a null result appears.
     */
    stdu    r1,-320(r1)
    std     r0,304(r1)
    mflr    r0
    std     r0,296(r1)
    std     r2,288(r1)
    std     r3,280(r1)
    std     r4,272(r1)
    std     r5,264(r1)
    std     r6,256(r1)
    std     r7,248(r1)
    std     r8,240(r1)
    std     r9,232(r1)
    std     r10,224(r1)
    std     r11,216(r1)
    std     r12,208(r1)
    mfcr    r12
    std     r12,200(r1)

    ld      r3,280(r1)
    ld      r4,272(r1)
    ld      r5,264(r1)
    lis     r12,0x003B
    ori     r12,r12,0xB70C
    mtctr   r12
    bctrl
    std     r3,192(r1)
    ld      r2,288(r1)

    ld      r11,264(r1)
    lwz     r10,0(r11)
    stw     r10,188(r1)

    ld      r11,272(r1)
    lis     r12,0x000A
    ori     r12,r12,0x00EC
    cmplw   cr7,r11,r12
    bc      BO_IF_TRUE,CR7_EQ,kimidori_lookup_diag_log
    cmpwi   cr7,r10,0
    bc      BO_IF_FALSE,CR7_EQ,kimidori_lookup_diag_done

kimidori_lookup_diag_log:
    lis     r12,0x5B74  /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206C  /* " lk " */
    ori     r12,r12,0x6B20
    stw     r12,36(r1)
    lis     r12,0x6B3D  /* "k=0x" */
    ori     r12,r12,0x3078
    stw     r12,40(r1)

    ld      r11,272(r1)
    STORE_HEX32 r11,44

    li      r12,32
    stb     r12,52(r1)
    li      r12,111
    stb     r12,53(r1)
    li      r12,61
    stb     r12,54(r1)
    li      r12,48
    stb     r12,55(r1)
    li      r12,120
    stb     r12,56(r1)

    lwz     r11,188(r1)
    STORE_HEX32 r11,57

    li      r12,32
    stb     r12,65(r1)
    li      r12,114
    stb     r12,66(r1)
    li      r12,61
    stb     r12,67(r1)
    li      r12,48
    stb     r12,68(r1)
    li      r12,120
    stb     r12,69(r1)

    ld      r11,192(r1)
    STORE_HEX32 r11,70

    li      r12,10
    stb     r12,78(r1)
    li      r12,0
    stw     r12,80(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,47
    addi    r6,r1,80
    li      r11,0x193
    sc      /* sys_tty_write */

kimidori_lookup_diag_done:
    ld      r2,288(r1)
    ld      r3,192(r1)
    addi    r1,r1,320

    lis     r12,0x003B
    ori     r12,r12,0x2524
    mtlr    r12
    mtctr   r12
    bctr

taiko_kimidori_dani_lookup_diag_hook_end:
    .size taiko_kimidori_dani_lookup_diag_hook_start, . - taiko_kimidori_dani_lookup_diag_hook_start

    .align 2
taiko_kimidori_dani_state4_service_diag_hook_start:
    /*
     * Hooked over Kimidori 0x00056858:
     *   lwz r10,0xD8(r3)
     *
     * Log state-4 service entry. The original load is replayed before
     * returning to 0x0005685C.
     */
    stdu    r1,-256(r1)
    std     r0,240(r1)
    std     r2,232(r1)
    std     r3,224(r1)
    std     r4,216(r1)
    std     r5,208(r1)
    std     r6,200(r1)
    std     r7,192(r1)
    std     r8,184(r1)
    std     r9,176(r1)
    std     r10,168(r1)
    std     r11,160(r1)
    std     r12,152(r1)
    mfcr    r12
    std     r12,144(r1)

    lis     r12,0x5B74  /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2073  /* " s4 " */
    ori     r12,r12,0x3420
    stw     r12,36(r1)
    lis     r12,0x613D  /* "a=0x" */
    ori     r12,r12,0x3078
    stw     r12,40(r1)

    ld      r11,224(r1)
    STORE_HEX32 r11,44

    li      r12,32
    stb     r12,52(r1)
    li      r12,116
    stb     r12,53(r1)
    li      r12,61
    stb     r12,54(r1)
    li      r12,48
    stb     r12,55(r1)
    li      r12,120
    stb     r12,56(r1)

    ld      r11,224(r1)
    lwz     r11,0xD8(r11)
    STORE_HEX32 r11,57

    li      r12,32
    stb     r12,65(r1)
    li      r12,99
    stb     r12,66(r1)
    li      r12,61
    stb     r12,67(r1)
    li      r12,48
    stb     r12,68(r1)
    li      r12,120
    stb     r12,69(r1)

    ld      r11,224(r1)
    lwz     r11,0xC0(r11)
    STORE_HEX32 r11,70

    li      r12,10
    stb     r12,78(r1)
    li      r12,0
    stw     r12,80(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,47
    addi    r6,r1,80
    li      r11,0x193
    sc      /* sys_tty_write */

    ld      r12,144(r1)
    mtcrf   0xff,r12
    ld      r12,152(r1)
    ld      r11,160(r1)
    ld      r9,176(r1)
    ld      r8,184(r1)
    ld      r7,192(r1)
    ld      r6,200(r1)
    ld      r5,208(r1)
    ld      r4,216(r1)
    ld      r3,224(r1)
    ld      r2,232(r1)
    ld      r0,240(r1)
    addi    r1,r1,256

    lwz     r10,0xD8(r3)
    lis     r12,0x0005
    ori     r12,r12,0x685C
    mtctr   r12
    bctr

taiko_kimidori_dani_state4_service_diag_hook_end:
    .size taiko_kimidori_dani_state4_service_diag_hook_start, . - taiko_kimidori_dani_state4_service_diag_hook_start
