    .section .rodata.taiko_patch_payload.green_lumen_scale,"a",@progbits
    .align 2
    .globl taiko_green_lumen_scale_hook_start
    .globl taiko_green_lumen_scale_hook_end
    .globl taiko_green_lumen_scale_addr
    .type taiko_green_lumen_scale_hook_start, @object

    .set r12,12
    .set f0,0
    .set f1,1
    .set f2,2

taiko_green_lumen_scale_hook_start:
    /* Green 0x38b56c: f1 is this movie's per-tick delta and f2 is its
     * accumulator.  Read a writable FPT float so the multiplier can be
     * adjusted live without another EBOOT patch. */
taiko_green_lumen_scale_addr:
    lis     r12,0x7a12
    ori     r12,r12,0x5ca1
    lfs     f0,0(r12)
    fmuls   f1,f1,f0
    fadds   f1,f2,f1           /* displaced instruction */

taiko_green_lumen_scale_hook_end:
    .size taiko_green_lumen_scale_hook_start, .-taiko_green_lumen_scale_hook_start
