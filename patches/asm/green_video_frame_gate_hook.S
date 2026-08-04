    .section .rodata.taiko_patch_payload.green_video_frame_gate,"a",@progbits
    .align 2
    .globl taiko_green_video_frame_gate_hook_start
    .globl taiko_green_video_frame_gate_hook_end
    .globl taiko_green_video_frame_gate_accum_addr
    .type taiko_green_video_frame_gate_hook_start, @object

    .set r0,0
    .set r1,1
    .set r2,2
    .set r3,3
    .set r4,4
    .set r10,10
    .set r11,11
    .set r12,12
    .set r31,31
    .set f0,0
    .set f1,1
    .set f2,2

taiko_green_video_frame_gate_hook_start:
    /* The stock game services CellSail once per 60 Hz update regardless of
     * whether the encoded movie is 25, 29.97, or 59.94 fps. CellSail owns
     * that source cadence and buffer lifecycle internally. Only suppress the
     * extra calls introduced by an unlocked vblank rate. */
taiko_green_video_frame_gate_accum_addr:
    lis     r12,0x7a12
    ori     r12,r12,0x5ca1
    lfs     f0,-4(r12)         /* animation_scale_bits: dt * 60 */
    lwz     r10,4(r12)         /* v14: wrapper entry count */
    addi    r10,r10,1
    stw     r10,4(r12)
    lis     r11,0x3f80         /* 1.0f */
    stw     r11,0x68(r1)
    lfs     f2,0x68(r1)
    fcmpu   cr7,f0,f2
    ble     cr7,taiko_green_video_frame_gate_scale_ready
    fmr     f0,f2              /* never accrue catch-up debt below 60 FPS */

taiko_green_video_frame_gate_scale_ready:
    lfs     f1,0(r12)          /* fractional source-frame budget */
    fadds   f1,f1,f0
    stfs    f1,0(r12)
    fcmpu   cr7,f1,f2
    blt     cr7,taiko_green_video_frame_gate_skip

    fsubs   f1,f1,f2
    stfs    f1,0(r12)
    lwz     r10,8(r12)         /* v14: permitted GetFrame count */
    addi    r10,r10,1
    stw     r10,8(r12)
    addi    r0,r31,0x20        /* displaced instruction */
    b       taiko_green_video_frame_gate_hook_end

taiko_green_video_frame_gate_skip:
    /* The caller keeps the last converted movie time at parent+0x790.
     * r31 is parent+0x410 here, so return that same time while retaining the
     * current decoded buffer at r31+0x120. */
    ld      r0,0x380(r31)
    std     r0,0x70(r1)
    lis     r11,0x0029
    ori     r11,r11,0x0294
    mtctr   r11
    bctr

taiko_green_video_frame_gate_hook_end:
    .size taiko_green_video_frame_gate_hook_start, .-taiko_green_video_frame_gate_hook_start
