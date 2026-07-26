    .section .rodata.taiko_patch_payload.pre_red_dani_emit_gate,"a",@progbits
    .align 2
    .globl taiko_pre_red_dani_emit_gate_hook_start
    .globl taiko_pre_red_dani_emit_gate_hook_end
    .type taiko_pre_red_dani_emit_gate_hook_start, @object

    .set r0,0
    .set r1,1
    .set r9,9
    .set r11,11
    .set r12,12
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_EQ,30

    .set PRE_RED_DANI_STATE_LOAD_MAGIC,0x0DAD1001
    .set PRE_RED_DANI_STATE_FIELD_MAGIC,0x0DAD1002
    .set PRE_RED_DANI_CONTINUE_LIS_MAGIC,0x0DAD2001
    .set PRE_RED_DANI_CONTINUE_ORI_MAGIC,0x0DAD2002
    .set PRE_RED_DANI_SKIP_LIS_MAGIC,0x0DAD3001
    .set PRE_RED_DANI_SKIP_ORI_MAGIC,0x0DAD3002

taiko_pre_red_dani_emit_gate_hook_start:
    /*
     * Hooked over the original type-9 emit branch:
     *   cmpwi  cr7,r0,9
     *   beq    cr7,skip_type9
     *   cmplwi cr7,r0,9
     *
     * Non-type-9 rows resume at the original cmplwi. Type-9 rows keep the
     * original first-song state restriction before entering the dormant case.
     */
    cmpwi   cr7,r0,9
    bc      BO_IF_FALSE,CR7_EQ,pre_red_dani_continue
    .long   PRE_RED_DANI_STATE_LOAD_MAGIC
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,pre_red_dani_skip
    clrldi  r11,r11,32
    .long   PRE_RED_DANI_STATE_FIELD_MAGIC
    cmpwi   cr7,r9,1
    bc      BO_IF_FALSE,CR7_EQ,pre_red_dani_skip

pre_red_dani_continue:
    .long   PRE_RED_DANI_CONTINUE_LIS_MAGIC
    .long   PRE_RED_DANI_CONTINUE_ORI_MAGIC
    mtctr   r12
    bctr

pre_red_dani_skip:
    .long   PRE_RED_DANI_SKIP_LIS_MAGIC
    .long   PRE_RED_DANI_SKIP_ORI_MAGIC
    mtctr   r12
    bctr

taiko_pre_red_dani_emit_gate_hook_end:
    .size taiko_pre_red_dani_emit_gate_hook_start, . - taiko_pre_red_dani_emit_gate_hook_start
