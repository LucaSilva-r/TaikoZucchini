    .section .rodata.taiko_patch_payload.green_don3d_scale,"a",@progbits
    .align 2
    .globl taiko_green_don3d_scale_hook_start
    .globl taiko_green_don3d_scale_hook_end
    .globl taiko_green_don3d_scale_addr
    .type taiko_green_don3d_scale_hook_start, @object

    .set r12,12
    .set f0,0
    .set f1,1
    .set f31,31

taiko_green_don3d_scale_hook_start:
    /* Green 0x2a6bcc: entry to the shared NU motion advance routine used by
     * every CDonAction state. Preserve its requested step in f31, then apply
     * the same live 60/120 multiplier used by the Lumen player. */
    fmr     f31,f1             /* displaced instruction */
taiko_green_don3d_scale_addr:
    lis     r12,0x7a12
    ori     r12,r12,0x5ca1
    lfs     f0,0(r12)
    fmuls   f31,f31,f0

taiko_green_don3d_scale_hook_end:
    .size taiko_green_don3d_scale_hook_start, .-taiko_green_don3d_scale_hook_start
