    .section .rodata.taiko_patch_payload.white_dani_taikojuku,"a",@progbits
    .align 2
    .globl taiko_white_dani_taikojuku_hook_start
    .globl taiko_white_dani_taikojuku_hook_end
    .type taiko_white_dani_taikojuku_hook_start, @object

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
    .set r19,19
    .set r21,21
    .set r29,29
    .set BO_IF_TRUE,12
    .set BO_IF_FALSE,4
    .set CR7_LT,28
    .set CR7_GT,29
    .set CR7_EQ,30

taiko_white_dani_taikojuku_hook_start:
    /* Prologue and original White 0x0D row append. */
    mflr    r0
    stdu    r1,-384(r1)
    std     r0,368(r1)
    std     r2,360(r1)

    /* Preserve original Dani visibility: row 0x0D only appears on song 1. */
    lwz     r11,0(r21)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,white_skip_dani_row
    clrldi  r11,r11,32
    lwz     r9,28(r11)
    cmpwi   cr7,r9,1
    bc      BO_IF_FALSE,CR7_EQ,white_skip_dani_row
    addi    r3,r1,504
    li      r4,13
    lwz     r5,16(r29)
    lwz     r6,20(r29)
    li      r7,-1
    lis     r12,0x0067
    ori     r12,r12,0x9DC8
    mtctr   r12
    bctrl
    ld      r2,360(r1)

white_skip_dani_row:
    /* Guard against duplicate visible row 0x11 in the local row vector. */
    li      r0,0
    stw     r0,64(r1)
    stw     r0,68(r1)
    stw     r0,72(r1)
    addi    r3,r1,504
    lwz     r4,0(r3)
    cmpwi   cr7,r4,0
    bc      BO_IF_TRUE,CR7_EQ,white_hook_finish
    clrldi  r4,r4,32
    lwz     r5,4(r4)
    lwz     r6,8(r4)
    cmplw   cr7,r5,r6
    bc      BO_IF_FALSE,CR7_LT,white_no_duplicate_row

white_duplicate_scan_loop:
    clrldi  r7,r5,32
    lwz     r8,0(r7)
    cmpwi   cr7,r8,17
    bc      BO_IF_TRUE,CR7_EQ,white_hook_finish
    addi    r5,r5,16
    cmplw   cr7,r5,r6
    bc      BO_IF_TRUE,CR7_LT,white_duplicate_scan_loop

white_no_duplicate_row:
    /* Resolve and validate the White selected-pack object. */
    lis     r12,0x0009
    ori     r12,r12,0x542C
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,white_hook_finish
    clrldi  r3,r3,32
    lwz     r0,0(r3)
    cmpwi   cr7,r0,2
    bc      BO_IF_FALSE,CR7_EQ,white_hook_finish
    li      r3,24
    lis     r12,0x0085
    ori     r12,r12,0xFCC8
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,white_hook_finish

    /* Initialize raw Taikojuku row state. */
    mr      r9,r3
    li      r0,0
    stw     r0,0(r9)
    stw     r0,4(r9)
    stw     r0,8(r9)
    li      r0,1
    stb     r0,20(r9)
    stb     r0,21(r9)
    li      r0,0
    stw     r0,64(r1)
    stw     r3,68(r1)
    stw     r0,72(r1)
    clrldi  r9,r3,32
    stw     r3,0(r9)
    stw     r3,4(r9)
    stw     r3,8(r9)

    /* Load selected raw pack number from White's +0x2D0 fields. */
    li      r10,1
    lwz     r11,8(r21)
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,white_pack_number_ready
    clrldi  r11,r11,32
    lbz     r0,0(r11)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,white_try_second_selected_pack
    lwz     r9,40(r11)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,white_try_second_selected_pack
    clrldi  r9,r9,32
    lwz     r10,720(r9)
    b       . + (white_pack_number_ready - .)

white_try_second_selected_pack:
    addi    r11,r11,44
    lbz     r0,0(r11)
    cmpwi   cr7,r0,0
    bc      BO_IF_TRUE,CR7_EQ,white_pack_number_ready
    lwz     r9,40(r11)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,white_pack_number_ready
    clrldi  r9,r9,32
    lwz     r10,720(r9)

white_pack_number_ready:
    cmpwi   cr7,r10,1
    bc      BO_IF_TRUE,CR7_LT,white_pack_number_default
    cmpwi   cr7,r10,10
    bc      BO_IF_FALSE,CR7_GT,white_resolve_raw_pack

white_pack_number_default:
    li      r10,1

white_resolve_raw_pack:
    /* Resolve raw pack through White helper 0x00954550. */
    addi    r3,r1,16
    mr      r4,r10
    lis     r12,0x0009
    ori     r12,r12,0x5450
    mtctr   r12
    bctrl
    ld      r2,360(r1)

    /* Clamp and copy the returned raw music list. */
    lwz     r8,16(r1)
    lwz     r9,20(r1)
    cmplw   cr7,r8,r9
    bc      BO_IF_FALSE,CR7_LT,white_skip_taikojuku_row
    stw     r8,24(r1)
    subf    r9,r8,r9
    srawi   r9,r9,3
    cmpwi   cr7,r9,0
    bc      BO_IF_FALSE,CR7_GT,white_skip_taikojuku_row
    cmpwi   cr7,r9,3
    bc      BO_IF_FALSE,CR7_GT,white_raw_music_count_ready
    li      r9,3

white_raw_music_count_ready:
    stw     r9,28(r1)
    li      r0,0
    stw     r0,48(r1)
    addi    r6,r1,128
    stw     r6,52(r1)
    rlwinm  r7,r9,3,0,28
    add     r7,r6,r7
    stw     r7,56(r1)
    stw     r7,60(r1)
    mtctr   r9
    li      r5,0

white_copy_raw_music_loop:
    lwz     r8,24(r1)
    clrldi  r8,r8,32
    lwz     r0,0(r8)
    stw     r0,0(r6)
    stw     r5,4(r6)
    addi    r8,r8,8
    stw     r8,24(r1)
    addi    r6,r6,8
    bdnz    white_copy_raw_music_loop

    /* Bind music indices through White helpers. */
    addi    r3,r1,64
    addi    r4,r1,48
    lis     r12,0x0008
    ori     r12,r12,0x7C88
    mtctr   r12
    bctrl
    ld      r2,360(r1)
    addi    r11,r1,64
    stw     r11,112(r1)
    addi    r3,r1,112
    addi    r4,r19,488
    lis     r12,0x0008
    ori     r12,r12,0x6B80
    mtctr   r12
    bctrl
    ld      r2,360(r1)

    /* Compute backing-vector append metadata and extra=-1 semantics. */
    lwz     r9,0(r21)
    cmpwi   cr7,r9,0
    bc      BO_IF_TRUE,CR7_EQ,white_skip_taikojuku_row
    clrldi  r10,r9,32
    lwz     r11,4(r10)
    li      r8,0
    cmpwi   cr7,r11,0
    bc      BO_IF_TRUE,CR7_EQ,white_backing_vector_count_ready
    lwz     r0,8(r10)
    subf    r8,r11,r0
    srawi   r8,r8,2

white_backing_vector_count_ready:
    addi    r7,r19,488
    lwz     r5,4(r7)
    li      r6,0
    cmpwi   cr7,r5,0
    bc      BO_IF_TRUE,CR7_EQ,white_bound_music_count_ready
    lwz     r0,8(r7)
    subf    r0,r5,r0
    srawi   r0,r0,2
    lis     r12,0x4F72
    ori     r12,r12,0xC235
    mullw   r6,r0,r12

white_bound_music_count_ready:
    li      r0,0
    stw     r0,80(r1)
    stw     r8,84(r1)
    li      r0,-1
    stw     r0,88(r1)
    stw     r9,92(r1)
    stw     r6,96(r1)
    addi    r3,r1,80
    addi    r4,r1,48
    lis     r12,0x0067
    ori     r12,r12,0xA2F0
    mtctr   r12
    bctrl
    ld      r2,360(r1)

    /* Append visible row 0x11 and clean row-local state. */
    lwz     r6,80(r1)
    cmpwi   cr7,r6,0
    bc      BO_IF_TRUE,CR7_EQ,white_skip_taikojuku_row
    addi    r3,r1,504
    li      r4,17
    lwz     r5,84(r1)
    extsw   r5,r5
    extsw   r6,r6
    li      r7,-1
    lis     r12,0x0067
    ori     r12,r12,0x9DC8
    mtctr   r12
    bctrl
    ld      r2,360(r1)

white_skip_taikojuku_row:
    lwz     r3,68(r1)
    cmpwi   cr7,r3,0
    bc      BO_IF_TRUE,CR7_EQ,white_hook_finish
    addi    r3,r1,64
    lis     r12,0x0067
    ori     r12,r12,0xAD7C
    mtctr   r12
    bctrl
    ld      r2,360(r1)

white_hook_finish:
    /* Epilogue and explicit jump to White row-iteration continuation. */
    ld      r0,368(r1)
    ld      r2,360(r1)
    mtlr    r0
    addi    r1,r1,384
    lis     r12,0x0067
    ori     r12,r12,0xDE40
    mtctr   r12
    bctr

taiko_white_dani_taikojuku_hook_end:
    .size taiko_white_dani_taikojuku_hook_start, . - taiko_white_dani_taikojuku_hook_start
