    .section .rodata.taiko_patch_payload.kimidori_dani_runtime_diag,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_lookup_diag_hook_start
    .globl taiko_kimidori_dani_lookup_diag_hook_end
    .globl taiko_kimidori_dani_state4_service_diag_hook_start
    .globl taiko_kimidori_dani_state4_service_diag_hook_end
    .globl taiko_kimidori_dani_registry_insert_diag_hook_start
    .globl taiko_kimidori_dani_registry_insert_diag_hook_end
    .globl taiko_kimidori_dani_registry_remove_diag_hook_start
    .globl taiko_kimidori_dani_registry_remove_diag_hook_end
    .globl taiko_kimidori_dani_registry_reset_diag_hook_start
    .globl taiko_kimidori_dani_registry_reset_diag_hook_end
    .type taiko_kimidori_dani_lookup_diag_hook_start, @object
    .type taiko_kimidori_dani_state4_service_diag_hook_start, @object
    .type taiko_kimidori_dani_registry_insert_diag_hook_start, @object
    .type taiko_kimidori_dani_registry_remove_diag_hook_start, @object
    .type taiko_kimidori_dani_registry_reset_diag_hook_start, @object

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

    .macro EMIT_REGISTRY_LINE op_lo, keyoff, valueoff, retoff, mapoff, lroff
    lis     r12,0x5B74  /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2072  /* " r?" */
    ori     r12,r12,\op_lo
    stw     r12,36(r1)
    lis     r12,0x6B3D  /* "k=0x" */
    ori     r12,r12,0x3078
    stw     r12,40(r1)

    ld      r11,\keyoff(r1)
    STORE_HEX32 r11,44

    li      r12,32
    stb     r12,52(r1)
    li      r12,118
    stb     r12,53(r1)
    li      r12,61
    stb     r12,54(r1)
    li      r12,48
    stb     r12,55(r1)
    li      r12,120
    stb     r12,56(r1)

    ld      r11,\valueoff(r1)
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

    ld      r11,\retoff(r1)
    STORE_HEX32 r11,70

    li      r12,32
    stb     r12,78(r1)
    li      r12,109
    stb     r12,79(r1)
    li      r12,61
    stb     r12,80(r1)
    li      r12,48
    stb     r12,81(r1)
    li      r12,120
    stb     r12,82(r1)

    ld      r11,\mapoff(r1)
    STORE_HEX32 r11,83

    li      r12,32
    stb     r12,91(r1)
    li      r12,108
    stb     r12,92(r1)
    li      r12,61
    stb     r12,93(r1)
    li      r12,48
    stb     r12,94(r1)
    li      r12,120
    stb     r12,95(r1)

    ld      r11,\lroff(r1)
    STORE_HEX32 r11,96

    li      r12,10
    stb     r12,104(r1)
    li      r12,0
    stw     r12,112(r1)

    li      r3,0
    addi    r4,r1,32
    li      r5,73
    addi    r6,r1,112
    li      r11,0x193
    sc      /* sys_tty_write */
    .endm

    .macro EMIT_RETAIN_LINE objoff, beforeoff, afteroff
    lis     r12,0x5B74  /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2072  /* " rt " */
    ori     r12,r12,0x7420
    stw     r12,36(r1)
    lis     r12,0x6F3D  /* "o=0x" */
    ori     r12,r12,0x3078
    stw     r12,40(r1)

    ld      r11,\objoff(r1)
    STORE_HEX32 r11,44

    li      r12,32
    stb     r12,52(r1)
    li      r12,98
    stb     r12,53(r1)
    li      r12,61
    stb     r12,54(r1)
    li      r12,48
    stb     r12,55(r1)
    li      r12,120
    stb     r12,56(r1)

    lwz     r11,\beforeoff(r1)
    STORE_HEX32 r11,57

    li      r12,32
    stb     r12,65(r1)
    li      r12,97
    stb     r12,66(r1)
    li      r12,61
    stb     r12,67(r1)
    li      r12,48
    stb     r12,68(r1)
    li      r12,120
    stb     r12,69(r1)

    lwz     r11,\afteroff(r1)
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
    .endm

    .macro EMIT_REMOVE_CALLER_LINE aoff, boff, coff
    lis     r12,0x5B74  /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2072  /* " rc " */
    ori     r12,r12,0x6320
    stw     r12,36(r1)
    lis     r12,0x613D  /* "a=0x" */
    ori     r12,r12,0x3078
    stw     r12,40(r1)

    ld      r11,\aoff(r1)
    STORE_HEX32 r11,44

    li      r12,32
    stb     r12,52(r1)
    li      r12,98
    stb     r12,53(r1)
    li      r12,61
    stb     r12,54(r1)
    li      r12,48
    stb     r12,55(r1)
    li      r12,120
    stb     r12,56(r1)

    ld      r11,\boff(r1)
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

    ld      r11,\coff(r1)
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
    .endm

taiko_kimidori_dani_lookup_diag_hook_start:
    /*
     * Hooked over Kimidori 0x003B2520:
     *   bl sub_3BB70C
     *
     * Preserve the original lookup call, then log the key, output pointer, and
     * boolean return when the 0x000Axxxx Dani resource family or a null result
     * appears.
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
    srwi    r12,r11,16
    cmpwi   cr7,r12,0x000A
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

    .align 2
taiko_kimidori_dani_registry_insert_diag_hook_start:
    /*
     * Hooked over Kimidori 0x003E6F14:
     *   bl sub_3BF608
     *
     * Trace insertions into the same registry family as the crashing lookup,
     * filtered to the 0x000Axxxx Dani resource family. r3=map, r4=key,
     * r5=value.
     * Dani retain experiment: keep one extra reference for the observed
     * 0x000A0000..0x000A019F resource family to prove whether the later final
     * release is the cause of the null lookup crashes.
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
    ld      r11,624(r1) /* parent frame saved LR */
    std     r11,320(r1)

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
    bc      BO_IF_FALSE,CR7_EQ,kimidori_registry_insert_done

    ld      r11,400(r1)
    lis     r12,0x000A
    ori     r12,r12,0x01A0
    cmplw   cr7,r11,r12
    bc      BO_IF_FALSE,CR7_LT,kimidori_registry_insert_emit

    ld      r11,312(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_registry_insert_emit

    ld      r11,392(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_registry_insert_emit

    addi    r9,r11,4
    clrldi  r9,r9,32
kimidori_registry_insert_retain_loop:
    lwarx   r10,0,r9
    addi    r8,r10,1
    stwcx.  r8,0,r9
    bc      BO_IF_FALSE,CR0_EQ,kimidori_registry_insert_retain_loop
    stw     r10,304(r1)
    stw     r8,296(r1)

kimidori_registry_insert_emit:
    EMIT_REGISTRY_LINE 0x6920,400,392,312,408,320
    ld      r11,400(r1)
    lis     r12,0x000A
    ori     r12,r12,0x01A0
    cmplw   cr7,r11,r12
    bc      BO_IF_FALSE,CR7_LT,kimidori_registry_insert_done
    ld      r11,312(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_registry_insert_done
    ld      r11,392(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_registry_insert_done
    EMIT_RETAIN_LINE 392,304,296

kimidori_registry_insert_done:
    ld      r2,416(r1)
    ld      r3,312(r1)
    addi    r1,r1,448

    lis     r12,0x003E
    ori     r12,r12,0x6F18
    mtctr   r12
    bctr

taiko_kimidori_dani_registry_insert_diag_hook_end:
    .size taiko_kimidori_dani_registry_insert_diag_hook_start, . - taiko_kimidori_dani_registry_insert_diag_hook_start

    .align 2
taiko_kimidori_dani_registry_remove_diag_hook_start:
    /*
     * Hooked over Kimidori 0x003E6DEC:
     *   bl sub_3BF430
     *
     * Trace removals from the same registry family as the crashing lookup,
     * filtered to the 0x000Axxxx Dani resource family. r3=map, r4=key.
     * The caller-chain line is only emitted for the known final-ref-release
     * path returning to 0x003E7300, where the backchain shape is known.
     */
    stdu    r1,-448(r1)
    std     r0,432(r1)
    mflr    r0
    std     r0,424(r1)
    std     r2,416(r1)
    std     r3,408(r1)
    std     r4,400(r1)
    li      r11,0
    std     r11,392(r1)
    std     r6,384(r1)
    std     r7,376(r1)
    std     r8,368(r1)
    std     r9,360(r1)
    std     r10,352(r1)
    std     r11,344(r1)
    std     r12,336(r1)
    mfcr    r12
    std     r12,328(r1)
    ld      r11,608(r1) /* parent frame saved LR */
    std     r11,320(r1)

    ld      r3,408(r1)
    ld      r4,400(r1)
    lis     r12,0x003B
    ori     r12,r12,0xF430
    mtctr   r12
    bctrl
    std     r3,312(r1)
    ld      r2,416(r1)

    ld      r11,400(r1)
    srwi    r12,r11,16
    cmpwi   cr7,r12,0x000A
    bc      BO_IF_FALSE,CR7_EQ,kimidori_registry_remove_done

    EMIT_REGISTRY_LINE 0x7220,400,392,312,408,320

    ld      r11,320(r1)
    lis     r12,0x003E
    ori     r12,r12,0x7300
    cmplw   cr7,r11,r12
    bc      BO_IF_FALSE,CR7_EQ,kimidori_registry_remove_done

    ld      r11,448(r1)   /* sub_3E6D6C backchain: sub_3E7284 frame */
    ld      r10,0(r11)    /* sub_3BB6E0 frame */
    ld      r9,0(r10)     /* sub_3BCC00 frame */
    ld      r8,0xB0(r11)  /* sub_3E7284 return to sub_3BB6E0 */
    std     r8,304(r1)
    ld      r8,0x80(r10)  /* sub_3BB6E0 return to sub_3BCC00 */
    std     r8,296(r1)
    ld      r8,0xA0(r9)   /* sub_3BCC00 caller */
    std     r8,288(r1)

    EMIT_REMOVE_CALLER_LINE 304,296,288

kimidori_registry_remove_done:
    ld      r2,416(r1)
    ld      r3,312(r1)
    addi    r1,r1,448

    lis     r12,0x003E
    ori     r12,r12,0x6DF0
    mtctr   r12
    bctr

taiko_kimidori_dani_registry_remove_diag_hook_end:
    .size taiko_kimidori_dani_registry_remove_diag_hook_start, . - taiko_kimidori_dani_registry_remove_diag_hook_start

    .align 2
taiko_kimidori_dani_registry_reset_diag_hook_start:
    /*
     * Hooked over Kimidori 0x003E6CE0:
     *   bl sub_3BF38C
     *
     * Trace bulk resets of the same registry family as the crashing lookup.
     * r3=map.
     */
    stdu    r1,-448(r1)
    std     r0,432(r1)
    mflr    r0
    std     r0,424(r1)
    std     r2,416(r1)
    std     r3,408(r1)
    li      r11,0
    std     r11,400(r1)
    std     r11,392(r1)
    std     r6,384(r1)
    std     r7,376(r1)
    std     r8,368(r1)
    std     r9,360(r1)
    std     r10,352(r1)
    std     r11,344(r1)
    std     r12,336(r1)
    mfcr    r12
    std     r12,328(r1)
    ld      r11,608(r1) /* parent frame saved LR */
    std     r11,320(r1)

    ld      r3,408(r1)
    lis     r12,0x003B
    ori     r12,r12,0xF38C
    mtctr   r12
    bctrl
    std     r3,312(r1)
    ld      r2,416(r1)

    EMIT_REGISTRY_LINE 0x7A20,400,392,312,408,320

    ld      r2,416(r1)
    ld      r3,312(r1)
    addi    r1,r1,448

    lis     r12,0x003E
    ori     r12,r12,0x6CE4
    mtctr   r12
    bctr

taiko_kimidori_dani_registry_reset_diag_hook_end:
    .size taiko_kimidori_dani_registry_reset_diag_hook_start, . - taiko_kimidori_dani_registry_reset_diag_hook_start
