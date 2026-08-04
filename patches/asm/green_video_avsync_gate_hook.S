    .section .rodata.taiko_patch_payload.green_video_avsync_gate,"a",@progbits
    .align 2
    .globl taiko_green_video_avsync_gate_hook_start
    .globl taiko_green_video_avsync_gate_hook_end
    .globl taiko_green_video_avsync_gate_accum_addr
    .type taiko_green_video_avsync_gate_hook_start, @object

    .set r1,1
    .set r3,3
    .set r10,10
    .set r11,11
    .set r12,12
    .set f0,0
    .set f1,1
    .set f2,2

taiko_green_video_avsync_gate_hook_start:
    /* Keep UpdateAvSync paired with the stock 60 Hz movie-service cadence.
     * Calling it on every unlocked render tick repeatedly reports the same
     * buffer tag and changes CellSail's decision about which PTS is due. */
taiko_green_video_avsync_gate_accum_addr:
    lis     r12,0x7a12
    ori     r12,r12,0x5ca1
    lfs     f0,-16(r12)        /* animation_scale_bits: dt * 60 */
    lwz     r10,4(r12)         /* v15: wrapper entry count */
    addi    r10,r10,1
    stw     r10,4(r12)

    lis     r11,0x3f80         /* 1.0f */
    stw     r11,0x68(r1)
    lfs     f2,0x68(r1)
    fcmpu   cr7,f0,f2
    ble     cr7,taiko_green_video_avsync_scale_ready
    fmr     f0,f2

taiko_green_video_avsync_scale_ready:
    lfs     f1,0(r12)
    fadds   f1,f1,f0
    stfs    f1,0(r12)
    fcmpu   cr7,f1,f2
    blt     cr7,taiko_green_video_avsync_skip

    fsubs   f1,f1,f2
    stfs    f1,0(r12)
    lwz     r10,8(r12)         /* v15: permitted UpdateAvSync count */
    addi    r10,r10,1
    stw     r10,8(r12)

    /* Displaced bl cellGcmGetLastFlipTime. An absolute call is required
     * because this payload is relocated away from the original call site. */
    lis     r11,0x00a1
    ori     r11,r11,0xcfd0
    mtctr   r11
    bctrl
    b       taiko_green_video_avsync_gate_hook_end

taiko_green_video_avsync_skip:
    lis     r11,0x0028
    ori     r11,r11,0xebc8     /* wrapper epilogue */
    mtctr   r11
    bctr

taiko_green_video_avsync_gate_hook_end:
    .size taiko_green_video_avsync_gate_hook_start, .-taiko_green_video_avsync_gate_hook_start
