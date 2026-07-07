    .section .rodata.taiko_patch_payload.momoiro_dani_state_diag,"a",@progbits
    .align 2
    .globl taiko_momoiro_dani_state10_set_diag_hook_start
    .globl taiko_momoiro_dani_state10_set_diag_hook_end
    .globl taiko_momoiro_dani_state10_result_diag_hook_start
    .globl taiko_momoiro_dani_state10_result_diag_hook_end
    .globl taiko_momoiro_dani_state10_prep_diag_hook_start
    .globl taiko_momoiro_dani_state10_prep_diag_hook_end
    .globl taiko_momoiro_dani_state10_after_queue_diag_hook_start
    .globl taiko_momoiro_dani_state10_after_queue_diag_hook_end
    .globl taiko_momoiro_dani_state11_expire_diag_hook_start
    .globl taiko_momoiro_dani_state11_expire_diag_hook_end
    .globl taiko_momoiro_dani_state8_result_diag_hook_start
    .globl taiko_momoiro_dani_state8_result_diag_hook_end
    .globl taiko_momoiro_dani_state8_to12_diag_hook_start
    .globl taiko_momoiro_dani_state8_to12_diag_hook_end
    .globl taiko_momoiro_dani_state12_entry_diag_hook_start
    .globl taiko_momoiro_dani_state12_entry_diag_hook_end
    .globl taiko_momoiro_dani_state12_mode_diag_hook_start
    .globl taiko_momoiro_dani_state12_mode_diag_hook_end
    .globl taiko_momoiro_dani_dojo_ctor4_diag_hook_start
    .globl taiko_momoiro_dani_dojo_ctor4_diag_hook_end
    .globl taiko_momoiro_dani_dojo_ctor5_diag_hook_start
    .globl taiko_momoiro_dani_dojo_ctor5_diag_hook_end
    .globl taiko_momoiro_dani_network_status_set_diag_hook_start
    .globl taiko_momoiro_dani_network_status_set_diag_hook_end
    .globl taiko_momoiro_dani_network_status2_diag_hook_start
    .globl taiko_momoiro_dani_network_status2_diag_hook_end
    .globl taiko_momoiro_dani_network_status1_diag_hook_start
    .globl taiko_momoiro_dani_network_status1_diag_hook_end
    .globl taiko_momoiro_dani_network_callback_success_diag_hook_start
    .globl taiko_momoiro_dani_network_callback_success_diag_hook_end
    .globl taiko_momoiro_dani_network_callback_result_diag_hook_start
    .globl taiko_momoiro_dani_network_callback_result_diag_hook_end
    .globl taiko_momoiro_dani_network_request_index_diag_hook_start
    .globl taiko_momoiro_dani_network_request_index_diag_hook_end
    .globl taiko_momoiro_dani_network_request_status2_diag_hook_start
    .globl taiko_momoiro_dani_network_request_status2_diag_hook_end
    .type taiko_momoiro_dani_state10_set_diag_hook_start, @object
    .type taiko_momoiro_dani_state10_result_diag_hook_start, @object
    .type taiko_momoiro_dani_state10_prep_diag_hook_start, @object
    .type taiko_momoiro_dani_state10_after_queue_diag_hook_start, @object
    .type taiko_momoiro_dani_state11_expire_diag_hook_start, @object
    .type taiko_momoiro_dani_state8_result_diag_hook_start, @object
    .type taiko_momoiro_dani_state8_to12_diag_hook_start, @object
    .type taiko_momoiro_dani_state12_entry_diag_hook_start, @object
    .type taiko_momoiro_dani_state12_mode_diag_hook_start, @object
    .type taiko_momoiro_dani_dojo_ctor4_diag_hook_start, @object
    .type taiko_momoiro_dani_dojo_ctor5_diag_hook_start, @object
    .type taiko_momoiro_dani_network_status_set_diag_hook_start, @object
    .type taiko_momoiro_dani_network_status2_diag_hook_start, @object
    .type taiko_momoiro_dani_network_status1_diag_hook_start, @object
    .type taiko_momoiro_dani_network_callback_success_diag_hook_start, @object
    .type taiko_momoiro_dani_network_callback_result_diag_hook_start, @object
    .type taiko_momoiro_dani_network_request_index_diag_hook_start, @object
    .type taiko_momoiro_dani_network_request_status2_diag_hook_start, @object

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
    .set r25,25
    .set r27,27
    .set r28,28
    .set r29,29
    .set r30,30
    .set r31,31
    .set BO_IF_TRUE,12
    .set CR7_EQ,30

    .macro SAVE_TTY_FRAME
    stdu    r1,-192(r1)
    std     r0,176(r1)
    std     r2,168(r1)
    std     r3,160(r1)
    std     r4,152(r1)
    std     r5,144(r1)
    std     r6,136(r1)
    std     r7,128(r1)
    std     r8,120(r1)
    std     r9,112(r1)
    std     r10,104(r1)
    std     r11,96(r1)
    mfcr    r12
    std     r12,88(r1)
    .endm

    .macro RESTORE_TTY_FRAME
    ld      r12,88(r1)
    mtcrf   0xff,r12
    ld      r11,96(r1)
    ld      r10,104(r1)
    ld      r9,112(r1)
    ld      r8,120(r1)
    ld      r7,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,176(r1)
    addi    r1,r1,192
    .endm

    .macro TTY_WRITE len
    li      r12,0
    stw     r12,64(r1)
    li      r3,0
    addi    r4,r1,32
    li      r5,\len
    addi    r6,r1,64
    li      r11,0x193
    sc
    .endm

    .macro STORE_HEX32 src, off
    mr      r7,\src
    addi    r8,r1,\off
    li      r9,8
1:
    srwi    r10,r7,28
    cmpwi   cr7,r10,10
    addi    r10,r10,48
    blt     cr7,2f
    addi    r10,r10,39
2:
    stb     r10,0(r8)
    slwi    r7,r7,4
    addi    r8,r8,1
    addi    r9,r9,-1
    cmpwi   cr7,r9,0
    bne     cr7,1b
    .endm

    .macro LOG_HEX_FIELD label_hi, label_lo, value_reg
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,\label_hi
    ori     r12,r12,\label_lo
    stw     r12,36(r1)
    lis     r12,0x3D30      /* "=0x" */
    ori     r12,r12,0x7800
    stw     r12,40(r1)
    STORE_HEX32 \value_reg,43
    li      r12,10
    stb     r12,51(r1)
    TTY_WRITE 20
    .endm

    .macro LOG_M8_STATUS
    lwz     r6,0x1D8(r27)
    LOG_HEX_FIELD 0x206D,0x3873,r6  /* " m8s" */
    .endm

    .macro LOG_NETWORK_STATUS_SET
    LOG_HEX_FIELD 0x206E,0x7320,r30  /* " ns " */
    .endm

    .macro LOG_NETWORK_STATUS2_FIELDS
    lbz     r6,0xAB(r31)
    LOG_HEX_FIELD 0x2068,0x3262,r6  /* " h2b" */
    lbz     r6,0xAA(r31)
    LOG_HEX_FIELD 0x2068,0x3261,r6  /* " h2a" */
    .endm

    .macro LOG_NETWORK_STATUS1_READY
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2068      /* " h1r" */
    ori     r12,r12,0x3172
    stw     r12,36(r1)
    li      r12,10
    stb     r12,40(r1)
    TTY_WRITE 9
    .endm

    .macro LOG_NETWORK_STATUS1_FIELDS
    lbz     r6,0xA8(r29)
    LOG_HEX_FIELD 0x2031,0x6138,r6  /* " 1a8" */
    lbz     r6,0xA9(r29)
    LOG_HEX_FIELD 0x2031,0x6139,r6  /* " 1a9" */
    lwz     r6,0x10(r29)
    LOG_HEX_FIELD 0x2076,0x3130,r6  /* " v10" */
    lwz     r6,0x14(r29)
    LOG_HEX_FIELD 0x2076,0x3134,r6  /* " v14" */
    lwz     r6,0x1C(r29)
    LOG_HEX_FIELD 0x2076,0x3163,r6  /* " v1c" */
    .endm

    .macro LOG_CALLBACK_SUCCESS
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2063      /* " cbo" */
    ori     r12,r12,0x626F
    stw     r12,36(r1)
    lis     r12,0x6B0A      /* "k\n" */
    stw     r12,40(r1)
    TTY_WRITE 10
    .endm

    .macro LOG_CALLBACK_RESULT
    LOG_HEX_FIELD 0x2063,0x6272,r3  /* " cbr" */
    .endm

    .macro LOG_REQUEST_INDEX
    LOG_HEX_FIELD 0x2072,0x7169,r3  /* " rqi" */
    .endm

    .macro LOG_REQUEST_STATUS2
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2072      /* " rq2" */
    ori     r12,r12,0x7132
    stw     r12,36(r1)
    li      r12,10
    stb     r12,40(r1)
    TTY_WRITE 9
    .endm

    .macro LOG_REQUEST_STATUS2_SKIP
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2072      /* " rqs" */
    ori     r12,r12,0x7173
    stw     r12,36(r1)
    lis     r12,0x6B0A      /* "k\n" */
    stw     r12,40(r1)
    TTY_WRITE 10
    .endm

    .macro LOG_Q_FIELD label_hi, label_lo, off
    addi    r6,r25,0xE4
    lwz     r6,\off(r6)
    LOG_HEX_FIELD \label_hi,\label_lo,r6
    .endm

    .macro LOG_M6_TO_10
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m6-" */
    ori     r12,r12,0x362D
    stw     r12,36(r1)
    lis     r12,0x3E31      /* ">10\n" */
    ori     r12,r12,0x300A
    stw     r12,40(r1)
    TTY_WRITE 12
    .endm

    .macro LOG_M10_OK
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m10" */
    ori     r12,r12,0x3130
    stw     r12,36(r1)
    lis     r12,0x206F      /* " ok\n" */
    ori     r12,r12,0x6B0A
    stw     r12,40(r1)
    TTY_WRITE 12
    .endm

    .macro LOG_M10_FAIL
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m10" */
    ori     r12,r12,0x3130
    stw     r12,36(r1)
    lis     r12,0x2066      /* " fai" */
    ori     r12,r12,0x6169
    stw     r12,40(r1)
    lis     r12,0x6C0A      /* "l\n" */
    stw     r12,44(r1)
    TTY_WRITE 14
    .endm

    .macro LOG_M10_PREP
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m10" */
    ori     r12,r12,0x3130
    stw     r12,36(r1)
    lis     r12,0x2070      /* " pre" */
    ori     r12,r12,0x7265
    stw     r12,40(r1)
    lis     r12,0x700A      /* "p\n" */
    stw     r12,44(r1)
    TTY_WRITE 14
    .endm

    .macro LOG_M11_TO_8
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m11" */
    ori     r12,r12,0x3131
    stw     r12,36(r1)
    lis     r12,0x2D3E      /* "->8\n" */
    ori     r12,r12,0x380A
    stw     r12,40(r1)
    TTY_WRITE 12
    .endm

    .macro LOG_M8_WAIT
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m8 " */
    ori     r12,r12,0x3820
    stw     r12,36(r1)
    lis     r12,0x7761      /* "wait" */
    ori     r12,r12,0x6974
    stw     r12,40(r1)
    lis     r12,0x0A00      /* "\n" */
    stw     r12,44(r1)
    TTY_WRITE 13
    .endm

    .macro LOG_M8_OK
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m8 " */
    ori     r12,r12,0x3820
    stw     r12,36(r1)
    lis     r12,0x6F6B      /* "ok\n" */
    ori     r12,r12,0x0A00
    stw     r12,40(r1)
    TTY_WRITE 11
    .endm

    .macro LOG_M8_FORCE
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m8 " */
    ori     r12,r12,0x3820
    stw     r12,36(r1)
    lis     r12,0x666F      /* "forc" */
    ori     r12,r12,0x7263
    stw     r12,40(r1)
    lis     r12,0x650A      /* "e\n" */
    stw     r12,44(r1)
    TTY_WRITE 14
    .endm

    .macro LOG_M8_TO_12
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m8-" */
    ori     r12,r12,0x382D
    stw     r12,36(r1)
    lis     r12,0x3E31      /* ">12\n" */
    ori     r12,r12,0x320A
    stw     r12,40(r1)
    TTY_WRITE 12
    .endm

    .macro LOG_M12_ENT
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m12" */
    ori     r12,r12,0x3132
    stw     r12,36(r1)
    lis     r12,0x2065      /* " ent" */
    ori     r12,r12,0x6E74
    stw     r12,40(r1)
    lis     r12,0x0A00      /* "\n" */
    stw     r12,44(r1)
    TTY_WRITE 13
    .endm

    .macro LOG_M12_MODE digit
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x206D      /* " m12" */
    ori     r12,r12,0x3132
    stw     r12,36(r1)
    .if \digit == 4
    lis     r12,0x206D      /* " m4\n" */
    ori     r12,r12,0x340A
    .elseif \digit == 5
    lis     r12,0x206D      /* " m5\n" */
    ori     r12,r12,0x350A
    .else
    lis     r12,0x206D      /* " m?\n" */
    ori     r12,r12,0x3F0A
    .endif
    stw     r12,40(r1)
    TTY_WRITE 12
    .endm

    .macro LOG_CTOR digit
    lis     r12,0x5B74      /* "[tz]" */
    ori     r12,r12,0x7A5D
    stw     r12,32(r1)
    lis     r12,0x2063      /* " cto" */
    ori     r12,r12,0x746F
    stw     r12,36(r1)
    .if \digit == 4
    lis     r12,0x7234      /* "r4\n" */
    .else
    lis     r12,0x7235      /* "r5\n" */
    .endif
    ori     r12,r12,0x0A00
    stw     r12,40(r1)
    TTY_WRITE 11
    .endm

taiko_momoiro_dani_state10_set_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B7894:
     *   li r9,4
     *
     * This is the row-0x0C/0x0D branch target after state 6. Re-emit the
     * original instruction after logging; 0x000B7898 still sets state 10.
     */
    SAVE_TTY_FRAME
    LOG_M6_TO_10
    RESTORE_TTY_FRAME

    li      r9,4
    lis     r12,0x000B
    ori     r12,r12,0x7898
    mtctr   r12
    bctr

taiko_momoiro_dani_state10_set_diag_hook_end:
    .size taiko_momoiro_dani_state10_set_diag_hook_start, . - taiko_momoiro_dani_state10_set_diag_hook_start

taiko_momoiro_dani_state10_result_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B69C0:
     *   beq cr7, loc_B70DC
     *
     * sub_72788 returns the readiness boolean in r3. Log the pass/fail split
     * and branch to the same native targets.
     */
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_state10_fail

momoiro_state10_ok:
    SAVE_TTY_FRAME
    LOG_M10_OK
    RESTORE_TTY_FRAME

    lis     r12,0x000B
    ori     r12,r12,0x69C4
    mtctr   r12
    bctr

momoiro_state10_fail:
    SAVE_TTY_FRAME
    LOG_M10_FAIL
    RESTORE_TTY_FRAME

    lis     r12,0x000B
    ori     r12,r12,0x70DC
    mtctr   r12
    bctr

taiko_momoiro_dani_state10_result_diag_hook_end:
    .size taiko_momoiro_dani_state10_result_diag_hook_start, . - taiko_momoiro_dani_state10_result_diag_hook_start

taiko_momoiro_dani_state10_prep_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B7AA4:
     *   addi r3,r25,0xE4
     *
     * For mode 4/5, state 10 takes this animation/setup branch before the
     * generic state-11 countdown.
     */
    SAVE_TTY_FRAME
    LOG_M10_PREP
    RESTORE_TTY_FRAME

    addi    r3,r25,0xE4
    lis     r12,0x000B
    ori     r12,r12,0x7AA8
    mtctr   r12
    bctr

taiko_momoiro_dani_state10_prep_diag_hook_end:
    .size taiko_momoiro_dani_state10_prep_diag_hook_start, . - taiko_momoiro_dani_state10_prep_diag_hook_start

taiko_momoiro_dani_state10_after_queue_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B7AB8:
     *   nop
     *
     * This runs immediately after sub_118BE0(this + 0xE4, 6, 7). Dump the
     * queue object fields that decide whether the resource request was active.
     */
    SAVE_TTY_FRAME
    LOG_Q_FIELD 0x2071,0x3034,0x04  /* " q04" */
    LOG_Q_FIELD 0x2071,0x3138,0x18  /* " q18" */
    LOG_Q_FIELD 0x2071,0x3263,0x2C  /* " q2c" */
    LOG_Q_FIELD 0x2071,0x3330,0x30  /* " q30" */
    LOG_Q_FIELD 0x2071,0x3334,0x34  /* " q34" */
    RESTORE_TTY_FRAME

    nop
    lis     r12,0x000B
    ori     r12,r12,0x7ABC
    mtctr   r12
    bctr

taiko_momoiro_dani_state10_after_queue_diag_hook_end:
    .size taiko_momoiro_dani_state10_after_queue_diag_hook_start, . - taiko_momoiro_dani_state10_after_queue_diag_hook_start

taiko_momoiro_dani_state11_expire_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B6A0C:
     *   li r0,-1
     *
     * This only runs when state 11's countdown expires and the machine writes
     * state 8 at 0x000B6A18.
     */
    SAVE_TTY_FRAME
    LOG_M11_TO_8
    RESTORE_TTY_FRAME

    li      r0,-1
    lis     r12,0x000B
    ori     r12,r12,0x6A10
    mtctr   r12
    bctr

taiko_momoiro_dani_state11_expire_diag_hook_end:
    .size taiko_momoiro_dani_state11_expire_diag_hook_start, . - taiko_momoiro_dani_state11_expire_diag_hook_start

taiko_momoiro_dani_state8_result_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B6660:
     *   beq cr7, loc_B5FF8
     *
     * sub_115B18 controls whether state 8 can advance to state 12.
     */
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,momoiro_state8_wait

momoiro_state8_ok:
    SAVE_TTY_FRAME
    LOG_M8_OK
    RESTORE_TTY_FRAME

    lis     r12,0x000B
    ori     r12,r12,0x6664
    mtctr   r12
    bctr

momoiro_state8_wait:
    SAVE_TTY_FRAME
    LOG_M8_WAIT
    LOG_M8_STATUS
    RESTORE_TTY_FRAME

    lis     r12,0x000B
    ori     r12,r12,0x5FF8
    mtctr   r12
    bctr

taiko_momoiro_dani_state8_result_diag_hook_end:
    .size taiko_momoiro_dani_state8_result_diag_hook_start, . - taiko_momoiro_dani_state8_result_diag_hook_start

taiko_momoiro_dani_state8_to12_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B6688:
     *   stw r0,0x10(r27)
     */
    SAVE_TTY_FRAME
    LOG_M8_TO_12
    RESTORE_TTY_FRAME

    stw     r0,0x10(r27)
    lis     r12,0x000B
    ori     r12,r12,0x668C
    mtctr   r12
    bctr

taiko_momoiro_dani_state8_to12_diag_hook_end:
    .size taiko_momoiro_dani_state8_to12_diag_hook_start, . - taiko_momoiro_dani_state8_to12_diag_hook_start

taiko_momoiro_dani_state12_entry_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B61B4:
     *   addi r3,r3,0xA4
     */
    SAVE_TTY_FRAME
    LOG_M12_ENT
    RESTORE_TTY_FRAME

    addi    r3,r3,0xA4
    lis     r12,0x000B
    ori     r12,r12,0x61B8
    mtctr   r12
    bctr

taiko_momoiro_dani_state12_entry_diag_hook_end:
    .size taiko_momoiro_dani_state12_entry_diag_hook_start, . - taiko_momoiro_dani_state12_entry_diag_hook_start

taiko_momoiro_dani_state12_mode_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B61C8:
     *   lwz r29,0x14(r27)
     *
     * Log which mode sub-switch state 12 is about to dispatch.
     */
    lwz     r29,0x14(r27)
    cmpwi   cr7,r29,4
    bc      BO_IF_TRUE,CR7_EQ,momoiro_state12_mode4
    cmpwi   cr7,r29,5
    bc      BO_IF_TRUE,CR7_EQ,momoiro_state12_mode5

momoiro_state12_mode_other:
    SAVE_TTY_FRAME
    LOG_M12_MODE 0
    RESTORE_TTY_FRAME
    b       momoiro_state12_mode_done

momoiro_state12_mode4:
    SAVE_TTY_FRAME
    LOG_M12_MODE 4
    RESTORE_TTY_FRAME
    b       momoiro_state12_mode_done

momoiro_state12_mode5:
    SAVE_TTY_FRAME
    LOG_M12_MODE 5
    RESTORE_TTY_FRAME

momoiro_state12_mode_done:
    lis     r12,0x000B
    ori     r12,r12,0x61CC
    mtctr   r12
    bctr

taiko_momoiro_dani_state12_mode_diag_hook_end:
    .size taiko_momoiro_dani_state12_mode_diag_hook_start, . - taiko_momoiro_dani_state12_mode_diag_hook_start

taiko_momoiro_dani_dojo_ctor4_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B6E74:
     *   bl sub_527880
     *
     * Mode 4 is the normal Dani Dojo row path. Log before preserving the
     * original DojoSelect constructor call.
     */
    SAVE_TTY_FRAME
    LOG_CTOR 4
    ld      r12,88(r1)
    mtcrf   0xff,r12
    ld      r11,96(r1)
    ld      r10,104(r1)
    ld      r9,112(r1)
    ld      r8,120(r1)
    ld      r7,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,176(r1)

    lis     r12,0x0052
    ori     r12,r12,0x7880
    mtctr   r12
    bctrl                       /* sub_527880 */
    ld      r2,168(r1)
    addi    r1,r1,192
    lis     r12,0x000B
    ori     r12,r12,0x6E78
    mtlr    r12
    mtctr   r12
    bctr

taiko_momoiro_dani_dojo_ctor4_diag_hook_end:
    .size taiko_momoiro_dani_dojo_ctor4_diag_hook_start, . - taiko_momoiro_dani_dojo_ctor4_diag_hook_start

taiko_momoiro_dani_dojo_ctor5_diag_hook_start:
    /*
     * Hooked over Momoiro 0x000B6C94:
     *   bl sub_527880
     *
     * Mode 5 is the adjacent Dani/Taikojuku-shaped path. It should not be the
     * normal row-0x0C path, but logging it rules out a mode mismatch.
     */
    SAVE_TTY_FRAME
    LOG_CTOR 5
    ld      r12,88(r1)
    mtcrf   0xff,r12
    ld      r11,96(r1)
    ld      r10,104(r1)
    ld      r9,112(r1)
    ld      r8,120(r1)
    ld      r7,128(r1)
    ld      r6,136(r1)
    ld      r5,144(r1)
    ld      r4,152(r1)
    ld      r3,160(r1)
    ld      r2,168(r1)
    ld      r0,176(r1)

    lis     r12,0x0052
    ori     r12,r12,0x7880
    mtctr   r12
    bctrl                       /* sub_527880 */
    ld      r2,168(r1)
    addi    r1,r1,192
    lis     r12,0x000B
    ori     r12,r12,0x6C98
    mtlr    r12
    mtctr   r12
    bctr

taiko_momoiro_dani_dojo_ctor5_diag_hook_end:
    .size taiko_momoiro_dani_dojo_ctor5_diag_hook_start, . - taiko_momoiro_dani_dojo_ctor5_diag_hook_start

taiko_momoiro_dani_network_status_set_diag_hook_start:
    /*
     * Hooked over Momoiro 0x001163D4:
     *   cmpwi cr7,r30,2
     *
     * sub_116360 is the wait-object status setter; r30 is the new status.
     */
    SAVE_TTY_FRAME
    LOG_NETWORK_STATUS_SET
    RESTORE_TTY_FRAME

    cmpwi   cr7,r30,2
    lis     r12,0x0011
    ori     r12,r12,0x63D8
    mtctr   r12
    bctr

taiko_momoiro_dani_network_status_set_diag_hook_end:
    .size taiko_momoiro_dani_network_status_set_diag_hook_start, . - taiko_momoiro_dani_network_status_set_diag_hook_start

taiko_momoiro_dani_network_status2_diag_hook_start:
    /*
     * Hooked over Momoiro 0x00116B58:
     *   lbz r0,0xAB(r31)
     *
     * Status 2 consumes +AB/+AA to choose the next wait-object state.
     */
    SAVE_TTY_FRAME
    LOG_NETWORK_STATUS2_FIELDS
    RESTORE_TTY_FRAME

    lbz     r0,0xAB(r31)
    lis     r12,0x0011
    ori     r12,r12,0x6B5C
    mtctr   r12
    bctr

taiko_momoiro_dani_network_status2_diag_hook_end:
    .size taiko_momoiro_dani_network_status2_diag_hook_start, . - taiko_momoiro_dani_network_status2_diag_hook_start

taiko_momoiro_dani_network_status1_diag_hook_start:
    /*
     * Hooked over Momoiro 0x001189B4:
     *   lbz r9,0xA8(r29)
     *
     * This is immediately after sub_118984 calls sub_116360(wait, 0).
     */
    SAVE_TTY_FRAME
    LOG_NETWORK_STATUS1_READY
    LOG_NETWORK_STATUS1_FIELDS
    RESTORE_TTY_FRAME

    lbz     r9,0xA8(r29)
    lis     r12,0x0011
    ori     r12,r12,0x89B8
    mtctr   r12
    bctr

taiko_momoiro_dani_network_status1_diag_hook_end:
    .size taiko_momoiro_dani_network_status1_diag_hook_start, . - taiko_momoiro_dani_network_status1_diag_hook_start

taiko_momoiro_dani_network_callback_success_diag_hook_start:
    /*
     * Hooked over Momoiro 0x0007B6D8:
     *   lwz r9,(off_AC36A0 - 0xAC8820)(r2)
     *
     * This callback sets wait +0xAB and clears wait +0xAA.
     */
    SAVE_TTY_FRAME
    LOG_CALLBACK_SUCCESS
    RESTORE_TTY_FRAME

    lwz     r9,-0x5180(r2)
    lis     r12,0x0007
    ori     r12,r12,0xB6DC
    mtctr   r12
    bctr

taiko_momoiro_dani_network_callback_success_diag_hook_end:
    .size taiko_momoiro_dani_network_callback_success_diag_hook_start, . - taiko_momoiro_dani_network_callback_success_diag_hook_start

taiko_momoiro_dani_network_callback_result_diag_hook_start:
    /*
     * Hooked over Momoiro 0x0007B7B0:
     *   lwz r9,(off_AC36A0 - 0xAC8820)(r2)
     *
     * This callback records non-success request results into wait +0xAA.
     */
    SAVE_TTY_FRAME
    LOG_CALLBACK_RESULT
    RESTORE_TTY_FRAME

    lwz     r9,-0x5180(r2)
    lis     r12,0x0007
    ori     r12,r12,0xB7B4
    mtctr   r12
    bctr

taiko_momoiro_dani_network_callback_result_diag_hook_end:
    .size taiko_momoiro_dani_network_callback_result_diag_hook_start, . - taiko_momoiro_dani_network_callback_result_diag_hook_start

taiko_momoiro_dani_network_request_index_diag_hook_start:
    /*
     * Hooked over Momoiro 0x00116790:
     *   lwz r10,0xE8(r28)
     *
     * This runs immediately after sub_115A64(wait + 0x0C), while r3 still
     * holds the request index selected for SongSelectNetwork::Request().
     */
    SAVE_TTY_FRAME
    LOG_REQUEST_INDEX
    RESTORE_TTY_FRAME

    lwz     r10,0xE8(r28)
    lis     r12,0x0011
    ori     r12,r12,0x6794
    mtctr   r12
    bctr

taiko_momoiro_dani_network_request_index_diag_hook_end:
    .size taiko_momoiro_dani_network_request_index_diag_hook_start, . - taiko_momoiro_dani_network_request_index_diag_hook_start

taiko_momoiro_dani_network_request_status2_diag_hook_start:
    /*
     * Hooked over Momoiro 0x00116A18:
     *   bl sub_116360
     *
     * This is the successful request submission path with r4 already set to 2.
     * In the Dani mode-4 state8 wait, status1 has just written ready status 0;
     * skip the immediate status2 re-arm so the native state8 check can advance.
     */
    SAVE_TTY_FRAME
    lwz     r6,-0x12C(r3)
    cmpwi   cr7,r6,8
    bne     cr7,momoiro_request_status2_native
    lwz     r6,-0x128(r3)
    cmpwi   cr7,r6,4
    bne     cr7,momoiro_request_status2_native

    LOG_REQUEST_STATUS2_SKIP
    RESTORE_TTY_FRAME

    lis     r12,0x0011
    ori     r12,r12,0x6A1C
    mtctr   r12
    bctr

momoiro_request_status2_native:
    LOG_REQUEST_STATUS2
    RESTORE_TTY_FRAME

    lis     r12,0x0011
    ori     r12,r12,0x6360
    mtctr   r12
    bctrl                       /* sub_116360 */
    lis     r12,0x0011
    ori     r12,r12,0x6A1C
    mtctr   r12
    bctr

taiko_momoiro_dani_network_request_status2_diag_hook_end:
    .size taiko_momoiro_dani_network_request_status2_diag_hook_start, . - taiko_momoiro_dani_network_request_status2_diag_hook_start
