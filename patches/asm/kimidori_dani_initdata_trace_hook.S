    .section .rodata.taiko_patch_payload.kimidori_dani_initdata_trace,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_initdata_trace_hook_start
    .globl taiko_kimidori_dani_initdata_trace_hook_end
    .type taiko_kimidori_dani_initdata_trace_hook_start, @object

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
    .set CR7_EQ,30
    .set CR7_LT,28

taiko_kimidori_dani_initdata_trace_hook_start:
    /*
     * Hooked over Kimidori nullsub_172 at 0x00215E24. InitData calls this
     * compiled-out logger immediately after pushing Dani rows/music into Lumen.
     * Keep it side-effect-free except for TTY so this remains a flow probe.
     */
    stdu    r1,-256(r1)
    std     r0,240(r1)
    mflr    r0
    std     r0,248(r1)
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

    ld      r11,224(r1)
    lis     r12,0x00A2
    ori     r12,r12,0x0DA0
    cmpw    cr7,r11,r12
    bc      BO_IF_TRUE,CR7_EQ,kimidori_trace_assign_dani
    lis     r12,0x00A2
    ori     r12,r12,0x0DD0
    cmpw    cr7,r11,r12
    bc      BO_IF_TRUE,CR7_EQ,kimidori_trace_set_dani_status
    lis     r12,0x00A2
    ori     r12,r12,0x0DF8
    cmpw    cr7,r11,r12
    bc      BO_IF_TRUE,CR7_EQ,kimidori_trace_assign_music
    lis     r12,0x00A2
    ori     r12,r12,0x0E38
    cmpw    cr7,r11,r12
    bc      BO_IF_TRUE,CR7_EQ,kimidori_trace_musicinfo_miss
    b       kimidori_trace_done

kimidori_trace_assign_dani:
    lis     r12,0x5B74
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2061
    ori     r12,r12,0x6420
    stw     r12,36(r1)
    lis     r12,0x723D
    ori     r12,r12,0x3078
    stw     r12,40(r1)
    ld      r7,216(r1)
    addi    r8,r1,44
    bl      kimidori_trace_hex8
    li      r12,10
    stb     r12,52(r1)
    li      r5,21
    bl      kimidori_trace_write
    b       kimidori_trace_done

kimidori_trace_set_dani_status:
    lis     r12,0x5B74
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2061
    ori     r12,r12,0x7320
    stw     r12,36(r1)
    lis     r12,0x633D
    ori     r12,r12,0x3078
    stw     r12,40(r1)
    ld      r7,216(r1)
    addi    r8,r1,44
    bl      kimidori_trace_hex8
    li      r12,32
    stb     r12,52(r1)
    li      r12,0x73
    stb     r12,53(r1)
    li      r12,0x3D
    stb     r12,54(r1)
    li      r12,0x30
    stb     r12,55(r1)
    li      r12,0x78
    stb     r12,56(r1)
    ld      r7,208(r1)
    addi    r8,r1,57
    bl      kimidori_trace_hex8
    li      r12,32
    stb     r12,65(r1)
    li      r12,0x72
    stb     r12,66(r1)
    li      r12,0x3D
    stb     r12,67(r1)
    li      r12,0x30
    stb     r12,68(r1)
    li      r12,0x78
    stb     r12,69(r1)
    ld      r7,200(r1)
    addi    r8,r1,70
    bl      kimidori_trace_hex8
    li      r12,10
    stb     r12,78(r1)
    li      r5,47
    bl      kimidori_trace_write
    b       kimidori_trace_done

kimidori_trace_assign_music:
    lis     r12,0x5B74
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2061
    ori     r12,r12,0x6D20
    stw     r12,36(r1)
    lis     r12,0x753D
    ori     r12,r12,0x3078
    stw     r12,40(r1)
    ld      r7,216(r1)
    addi    r8,r1,44
    bl      kimidori_trace_hex8
    li      r12,32
    stb     r12,52(r1)
    li      r12,0x67
    stb     r12,53(r1)
    li      r12,0x3D
    stb     r12,54(r1)
    li      r12,0x30
    stb     r12,55(r1)
    li      r12,0x78
    stb     r12,56(r1)
    ld      r7,208(r1)
    addi    r8,r1,57
    bl      kimidori_trace_hex8
    li      r12,32
    stb     r12,65(r1)
    li      r12,0x63
    stb     r12,66(r1)
    li      r12,0x3D
    stb     r12,67(r1)
    li      r12,0x30
    stb     r12,68(r1)
    li      r12,0x78
    stb     r12,69(r1)
    ld      r7,200(r1)
    addi    r8,r1,70
    bl      kimidori_trace_hex8
    li      r12,32
    stb     r12,78(r1)
    li      r12,0x73
    stb     r12,79(r1)
    li      r12,0x3D
    stb     r12,80(r1)
    li      r12,0x30
    stb     r12,81(r1)
    li      r12,0x78
    stb     r12,82(r1)
    ld      r7,192(r1)
    addi    r8,r1,83
    bl      kimidori_trace_hex8
    li      r12,10
    stb     r12,91(r1)
    li      r5,60
    bl      kimidori_trace_write
    b       kimidori_trace_done

kimidori_trace_musicinfo_miss:
    lis     r12,0x5B74
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2061
    ori     r12,r12,0x6620
    stw     r12,36(r1)
    lis     r12,0x753D
    ori     r12,r12,0x3078
    stw     r12,40(r1)
    ld      r7,216(r1)
    addi    r8,r1,44
    bl      kimidori_trace_hex8
    li      r12,10
    stb     r12,52(r1)
    li      r5,21
    bl      kimidori_trace_write
    b       kimidori_trace_done

kimidori_trace_hex8:
    li      r9,8
kimidori_trace_hex8_loop:
    rlwinm  r10,r7,4,28,31
    cmpwi   cr7,r10,10
    addi    r10,r10,0x30
    bc      BO_IF_TRUE,CR7_LT,kimidori_trace_hex8_store
    addi    r10,r10,0x27
kimidori_trace_hex8_store:
    stb     r10,0(r8)
    rlwinm  r7,r7,4,0,27
    addi    r8,r8,1
    addi    r9,r9,-1
    cmpwi   cr7,r9,0
    bne     cr7,kimidori_trace_hex8_loop
    blr

kimidori_trace_write:
    li      r12,0
    stw     r12,128(r1)
    li      r3,0
    addi    r4,r1,32
    addi    r6,r1,128
    li      r11,0x0193
    sc
    blr

kimidori_trace_done:
    ld      r12,144(r1)
    mtcr    r12
    ld      r12,152(r1)
    ld      r11,160(r1)
    ld      r10,168(r1)
    ld      r9,176(r1)
    ld      r8,184(r1)
    ld      r7,192(r1)
    ld      r6,200(r1)
    ld      r5,208(r1)
    ld      r4,216(r1)
    ld      r3,224(r1)
    ld      r2,232(r1)
    ld      r0,248(r1)
    mtlr    r0
    ld      r0,240(r1)
    addi    r1,r1,256
    blr

taiko_kimidori_dani_initdata_trace_hook_end:
    .size taiko_kimidori_dani_initdata_trace_hook_start, . - taiko_kimidori_dani_initdata_trace_hook_start
