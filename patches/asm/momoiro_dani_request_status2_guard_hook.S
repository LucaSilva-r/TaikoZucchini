    .section .rodata.taiko_patch_payload.momoiro_dani_request_status2_guard,"a",@progbits
    .align 2
    .globl taiko_momoiro_dani_request_status2_guard_hook_start
    .globl taiko_momoiro_dani_request_status2_guard_hook_end
    .type taiko_momoiro_dani_request_status2_guard_hook_start, @object

    .set r3,3
    .set r6,6
    .set r12,12

taiko_momoiro_dani_request_status2_guard_hook_start:
    /*
     * Hooked over Momoiro 0x00116A18:
     *   bl sub_116360
     *
     * The status1 completion chain can immediately re-submit request index 0
     * and set status 2 again while Dani state8 is polling. In mode4/state8,
     * skip only that re-arm so the native state8 ready check observes status 0.
     *
     * wait == state + 0x13C:
     *   wait - 0x12C == state + 0x10
     *   wait - 0x128 == state + 0x14
     */
    lwz     r6,-0x12C(r3)
    cmpwi   cr7,r6,8
    bne     cr7,momoiro_request_status2_native
    lwz     r6,-0x128(r3)
    cmpwi   cr7,r6,4
    bne     cr7,momoiro_request_status2_native

    lis     r12,0x0011
    ori     r12,r12,0x6A1C
    mtctr   r12
    bctr

momoiro_request_status2_native:
    lis     r12,0x0011
    ori     r12,r12,0x6360
    mtctr   r12
    bctrl                       /* sub_116360 */
    lis     r12,0x0011
    ori     r12,r12,0x6A1C
    mtctr   r12
    bctr

taiko_momoiro_dani_request_status2_guard_hook_end:
    .size taiko_momoiro_dani_request_status2_guard_hook_start, . - taiko_momoiro_dani_request_status2_guard_hook_start
