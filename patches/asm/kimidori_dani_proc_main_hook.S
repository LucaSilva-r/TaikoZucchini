    .section .rodata.taiko_patch_payload.kimidori_dani_proc_main,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_proc_main_hook_start
    .globl taiko_kimidori_dani_proc_main_hook_end
    .type taiko_kimidori_dani_proc_main_hook_start, @object

    .set r0,0
    .set r12,12
    .set BO_IF_TRUE,12
    .set CR7_EQ,30

taiko_kimidori_dani_proc_main_hook_start:
    /*
     * Hooked over Kimidori 0x005666C:
     *   cmpwi cr7,r0,0x1A
     *
     * The restored row hook emits marker 0x0D. Preserve native marker 0x1A
     * and route both markers to the existing Dani branch target. Every other
     * marker resumes the original non-Dani path at 0x0056674.
     */
    cmpwi   cr7,r0,0x1A
    bc      BO_IF_TRUE,CR7_EQ,kimidori_proc_main_dani
    cmpwi   cr7,r0,0x0D
    bc      BO_IF_TRUE,CR7_EQ,kimidori_proc_main_dani

    lis     r12,0x0005
    ori     r12,r12,0x6674
    mtctr   r12
    bctr

kimidori_proc_main_dani:
    lis     r12,0x0005
    ori     r12,r12,0x66E8
    mtctr   r12
    bctr

taiko_kimidori_dani_proc_main_hook_end:
    .size taiko_kimidori_dani_proc_main_hook_start, . - taiko_kimidori_dani_proc_main_hook_start
