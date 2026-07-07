    .section .rodata.taiko_patch_payload.momoiro_dani_emit_select,"a",@progbits
    .align 2
    .globl taiko_momoiro_dani_emit_gate_hook_start
    .globl taiko_momoiro_dani_emit_gate_hook_end
    .type taiko_momoiro_dani_emit_gate_hook_start, @object

    .set r0,0
    .set r12,12

taiko_momoiro_dani_emit_gate_hook_start:
    /*
     * Hooked over Momoiro 0x00528540:
     *   beq cr7, loc_5285D0
     *
     * Force type-9 rows through Momoiro's native not-taken path. The dormant
     * row entry at 0x005293F8 then emits the Dani row as type 0x0C.
     */
    cmpwi   cr7,r0,9
    lis     r12,0x0052
    ori     r12,r12,0x8544
    mtctr   r12
    bctr

taiko_momoiro_dani_emit_gate_hook_end:
    .size taiko_momoiro_dani_emit_gate_hook_start, . - taiko_momoiro_dani_emit_gate_hook_start
