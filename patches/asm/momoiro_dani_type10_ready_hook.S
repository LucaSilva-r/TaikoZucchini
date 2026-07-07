    .section .rodata.taiko_patch_payload.momoiro_dani_type10_ready,"a",@progbits
    .align 2
    .globl taiko_momoiro_dani_type10_ready_hook_start
    .globl taiko_momoiro_dani_type10_ready_hook_end
    .type taiko_momoiro_dani_type10_ready_hook_start, @object

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
    .set r29,29
    .set r30,30
    .set r31,31
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_EQ,30
    .set CR7_GT,29

taiko_momoiro_dani_type10_ready_hook_start:
    /*
     * Hooked over Momoiro 0x0002E414:
     *   nop
     *
     * Momoiro's type-10 lookup tree lives at root + 0x220. Dani Select can
     * request title/fillrect resources before the queued ranges are expanded,
     * so the lookup returns the dummy 0x000A0000 resource. In that exact state,
     * run Momoiro's native root resource builder and resolve the requested UID
     * again before the draw call consumes r31.
     */
    stdu    r1,-384(r1)
    std     r0,368(r1)
    mflr    r0
    std     r0,376(r1)
    std     r2,360(r1)
    std     r3,352(r1)
    std     r4,344(r1)
    std     r5,336(r1)
    std     r6,328(r1)
    std     r7,320(r1)
    std     r8,312(r1)
    std     r9,304(r1)
    std     r10,296(r1)
    std     r11,288(r1)
    std     r12,280(r1)
    mfcr    r12
    std     r12,272(r1)
    std     r29,264(r1)
    std     r30,256(r1)
    std     r31,248(r1)

    lis     r12,0x000A
    cmpw    cr7,r31,r12
    bc      BO_IF_FALSE,CR7_EQ,momoiro_type10_ready_done

    /*
     * Original RequestFillrect frame is 0x180 bytes above this hook frame.
     * n2_1 at old sp+0x7c says whether the uid was an int or float.
     */
    lwz     r11,0x1FC(r1)
    cmpwi   cr7,r11,2
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_uid_int
    lwz     r11,0x220(r1)       /* converted float uid */
    b       momoiro_type10_uid_ready

momoiro_type10_uid_int:
    lwz     r11,0x200(r1)       /* raw integer uid */

momoiro_type10_uid_ready:
    srawi   r11,r11,1
    stw     r11,236(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done

    lwz     r11,0x1F0(r1)       /* root local passed to sub_7CD20 */
    stw     r11,240(r1)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done

    clrldi  r11,r11,32
    lwz     r10,0x220(r11)      /* type tree root */
    cmpwi   cr7,r10,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done

    mr      r8,r10              /* sentinel/root */
    clrldi  r9,r10,32
    lwz     r10,4(r9)           /* current node */
    mr      r7,r8               /* candidate */

momoiro_type10_outer:
    clrldi  r12,r10,32
    lbz     r0,0x31(r12)
    cmpwi   cr7,r0,0
    bc      BO_IF_FALSE,CR7_EQ,momoiro_type10_finish

momoiro_type10_inner:
    lwz     r0,0x0C(r12)
    cmplwi  cr7,r0,9
    bc      BO_IF_TRUE,CR7_GT,momoiro_type10_candidate
    lwz     r10,8(r12)
    clrldi  r12,r10,32
    lbz     r0,0x31(r12)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_inner
    b       momoiro_type10_finish

momoiro_type10_candidate:
    mr      r7,r10
    lwz     r10,0(r12)
    b       momoiro_type10_outer

momoiro_type10_finish:
    cmpw    cr7,r8,r7
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done
    clrldi  r7,r7,32
    lwz     r0,0x0C(r7)
    cmplwi  cr7,r0,10
    bc      BO_IF_TRUE,CR7_GT,momoiro_type10_ready_done

    addi    r9,r7,0x10
    lwz     r11,0x14(r9)        /* range begin */
    lwz     r12,0x18(r9)        /* range end */
    cmpw    cr7,r11,r12
    bc      BO_IF_FALSE,CR7_EQ,momoiro_type10_ready_done

    lwz     r3,240(r1)
    clrldi  r3,r3,32
    lis     r12,0x0002
    ori     r12,r12,0x2FC4
    mtctr   r12
    bctrl                       /* sub_22FC4(root) */
    ld      r2,360(r1)

    lwz     r11,240(r1)
    stw     r11,224(r1)
    addi    r3,r1,224
    li      r4,10
    lwz     r5,236(r1)
    lis     r12,0x0007
    ori     r12,r12,0xCD20
    mtctr   r12
    bctrl                       /* sub_7CD20(&root, 10, uid) */
    clrldi  r3,r3,32
    ld      r2,360(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done
    lis     r12,0x000A
    cmpw    cr7,r3,r12
    bc      BO_IF_TRUE,CR7_EQ,momoiro_type10_ready_done
    std     r3,248(r1)          /* restored r31 feeds sub_309FC */

momoiro_type10_ready_done:
    ld      r31,248(r1)
    ld      r30,256(r1)
    ld      r29,264(r1)
    ld      r12,272(r1)
    mtcrf   0xff,r12
    ld      r12,280(r1)
    ld      r11,288(r1)
    ld      r10,296(r1)
    ld      r9,304(r1)
    ld      r8,312(r1)
    ld      r7,320(r1)
    ld      r6,328(r1)
    ld      r5,336(r1)
    ld      r4,344(r1)
    ld      r3,352(r1)
    ld      r2,360(r1)
    ld      r0,376(r1)
    mtlr    r0
    ld      r0,368(r1)
    addi    r1,r1,384

    lis     r12,0x0002
    ori     r12,r12,0xE418
    mtctr   r12
    bctr

taiko_momoiro_dani_type10_ready_hook_end:
    .size taiko_momoiro_dani_type10_ready_hook_start, . - taiko_momoiro_dani_type10_ready_hook_start
