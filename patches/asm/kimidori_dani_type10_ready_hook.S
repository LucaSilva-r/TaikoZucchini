    .section .rodata.taiko_patch_payload.kimidori_dani_type10_ready,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_type10_ready_hook_start
    .globl taiko_kimidori_dani_type10_ready_hook_end
    .type taiko_kimidori_dani_type10_ready_hook_start, @object

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
    .set r28,28
    .set r29,29
    .set r31,31
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_EQ,30
    .set CR7_GT,29

taiko_kimidori_dani_type10_ready_hook_start:
    /*
     * Hooked over Kimidori 0x0002F85C:
     *   nop
     *
     * Dani Select can request type-10 fillrect/title resources before
     * Kimidori has expanded the queued package list into lookup ranges. When
     * the exact broken type-10 state is detected, run Kimidori's native
     * type-entry builder and re-resolve the requested UID before the original
     * draw call consumes r29.
     */
    stdu    r1,-320(r1)
    std     r0,304(r1)
    mflr    r0
    std     r0,312(r1)
    std     r2,296(r1)
    std     r3,288(r1)
    std     r4,280(r1)
    std     r5,272(r1)
    std     r6,264(r1)
    std     r7,256(r1)
    std     r8,248(r1)
    std     r9,240(r1)
    std     r10,232(r1)
    std     r11,224(r1)
    std     r12,216(r1)
    mfcr    r12
    std     r12,208(r1)
    std     r28,200(r1)
    std     r29,192(r1)
    std     r31,184(r1)

    lwz     r11,440(r1)     /* original RequestFillrect frame: sp+0x78 uid */
    stw     r11,172(r1)
    stw     r29,168(r1)     /* current resolved fillrect id */
    lwz     r10,0(r28)      /* dword_C0422C */
    li      r11,0
    li      r12,0
    cmpwi   cr7,r10,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_root_done
    lwz     r11,0(r10)      /* *(dword_C0422C + 0): resource catalog root */
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_root_done
    lwz     r12,512(r11)    /* *(resource catalog root + 0x200): type tree */
kimidori_type10_root_done:
    stw     r11,160(r1)
    stw     r12,156(r1)

    li      r11,0
    stw     r11,148(r1)     /* type-10 entry */
    stw     r11,140(r1)     /* packed flags: type,b8,b9,b10 */
    stw     r11,136(r1)     /* range vector begin */
    stw     r11,132(r1)     /* range vector end */
    stw     r11,120(r1)     /* package byte count */

    lwz     r11,156(r1)     /* type tree root */
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done
    clrldi  r11,r11,32
    lwz     r9,4(r11)
    mr      r10,r11

kimidori_type10_outer:
    clrldi  r12,r9,32
    lbz     r0,0x3D(r12)
    cmpwi   cr7,r0,0
    bc      BO_IF_FALSE,CR7_EQ,kimidori_type10_finish

kimidori_type10_inner:
    lwz     r0,0x0C(r12)
    cmplwi  cr7,r0,9
    bc      BO_IF_TRUE,CR7_GT,kimidori_type10_candidate
    lwz     r9,8(r12)
    clrldi  r12,r9,32
    lbz     r0,0x3D(r12)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_inner
    b       kimidori_type10_finish

kimidori_type10_candidate:
    mr      r10,r9
    lwz     r9,0(r12)
    b       kimidori_type10_outer

kimidori_type10_finish:
    cmpw    cr7,r11,r10
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done
    clrldi  r10,r10,32
    lwz     r0,0x0C(r10)
    cmplwi  cr7,r0,10
    bc      BO_IF_TRUE,CR7_GT,kimidori_type10_ready_done

    addi    r9,r10,0x10
    stw     r9,148(r1)

    clrldi  r10,r9,32
    li      r11,10
    slwi    r11,r11,24
    lbz     r12,8(r10)
    slwi    r12,r12,16
    or      r11,r11,r12
    lbz     r12,9(r10)
    slwi    r12,r12,8
    or      r11,r11,r12
    lbz     r12,10(r10)
    or      r11,r11,r12
    stw     r11,140(r1)

    lwz     r11,16(r10)
    stw     r11,136(r1)
    lwz     r12,20(r10)
    stw     r12,132(r1)
    lwz     r11,32(r10)
    lwz     r12,36(r10)
    subf    r11,r11,r12
    stw     r11,120(r1)

    lwz     r11,148(r1)     /* type-10 entry */
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done

    lwz     r11,172(r1)     /* requested uid */
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done

    lwz     r11,168(r1)     /* current resolved fillrect id */
    lis     r12,0x000A
    cmpw    cr7,r11,r12
    bc      BO_IF_FALSE,CR7_EQ,kimidori_type10_ready_done

    lwz     r11,140(r1)     /* type,b8,b9,b10 */
    lis     r12,0x0A01
    ori     r12,r12,0x0100
    cmpw    cr7,r11,r12
    bc      BO_IF_FALSE,CR7_EQ,kimidori_type10_ready_done

    lwz     r11,120(r1)     /* package byte count */
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done

    lwz     r11,136(r1)     /* range begin */
    lwz     r12,132(r1)     /* range end */
    cmpw    cr7,r11,r12
    bc      BO_IF_FALSE,CR7_EQ,kimidori_type10_ready_done

    lwz     r3,148(r1)
    clrldi  r3,r3,32
    lis     r12,0x0009
    ori     r12,r12,0x3258
    mtctr   r12
    bctrl                       /* sub_93258(type10 entry) */
    ld      r2,296(r1)

    lwz     r11,160(r1)
    stw     r11,104(r1)     /* local root pointer for sub_92620 */
    addi    r3,r1,104
    li      r4,10
    lwz     r5,172(r1)
    lis     r12,0x0009
    ori     r12,r12,0x2620
    mtctr   r12
    bctrl                       /* sub_92620(&root, 10, uid) */
    clrldi  r3,r3,32
    ld      r2,296(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,kimidori_type10_ready_done
    std     r3,192(r1)      /* restored r29 feeds the original draw call */

kimidori_type10_ready_done:
    ld      r31,184(r1)
    ld      r29,192(r1)
    ld      r28,200(r1)
    ld      r12,208(r1)
    mtcrf   0xff,r12
    ld      r12,216(r1)
    ld      r11,224(r1)
    ld      r10,232(r1)
    ld      r9,240(r1)
    ld      r8,248(r1)
    ld      r7,256(r1)
    ld      r6,264(r1)
    ld      r5,272(r1)
    ld      r4,280(r1)
    ld      r3,288(r1)
    ld      r2,296(r1)
    ld      r0,312(r1)
    mtlr    r0
    ld      r0,304(r1)
    addi    r1,r1,320

    lis     r12,0x0002
    ori     r12,r12,0xF860
    mtctr   r12
    bctr

taiko_kimidori_dani_type10_ready_hook_end:
    .size taiko_kimidori_dani_type10_ready_hook_start, . - taiko_kimidori_dani_type10_ready_hook_start
