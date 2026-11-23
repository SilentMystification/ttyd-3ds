@ qtmbench memory kernels (ARM mode). Byte counts are multiples of 64.
@ SPDX-License-Identifier: MIT

    .arm
    .syntax unified
    .text

@ void k_read_ldm(const void* p, u32 bytes)
    .global k_read_ldm
    .type k_read_ldm, %function
    .balign 32
k_read_ldm:
    push {r4-r10, lr}
    add r1, r0, r1
1:  ldm r0!, {r2-r9}
    ldm r0!, {r2-r9}
    cmp r0, r1
    blo 1b
    pop {r4-r10, pc}
    .size k_read_ldm, .-k_read_ldm

@ void k_write_stm(void* p, u32 bytes, u32 value)
    .global k_write_stm
    .type k_write_stm, %function
    .balign 32
k_write_stm:
    push {r4-r10, lr}
    add r1, r0, r1
    mov r3, r2
    mov r4, r2
    mov r5, r2
    mov r6, r2
    mov r7, r2
    mov r8, r2
    mov r9, r2
1:  stm r0!, {r2-r9}
    stm r0!, {r2-r9}
    cmp r0, r1
    blo 1b
    pop {r4-r10, pc}
    .size k_write_stm, .-k_write_stm

@ void k_copy_ldmstm(void* dst, const void* src, u32 bytes)
    .global k_copy_ldmstm
    .type k_copy_ldmstm, %function
    .balign 32
k_copy_ldmstm:
    push {r4-r11, lr}
    add r2, r1, r2
1:  ldm r1!, {r3-r10}
    stm r0!, {r3-r10}
    ldm r1!, {r3-r10}
    stm r0!, {r3-r10}
    cmp r1, r2
    blo 1b
    pop {r4-r11, pc}
    .size k_copy_ldmstm, .-k_copy_ldmstm

@ const void* k_chase(const void* p, u32 loads)   loads multiple of 8
    .global k_chase
    .type k_chase, %function
    .balign 32
k_chase:
1:  ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    ldr r0, [r0]
    subs r1, r1, #8
    bhi 1b
    bx lr
    .size k_chase, .-k_chase

@ u32 k_rand_read(const u32* base, u32 mask, u32 loads, u32 seed)
@ Independent loads at LCG addresses (word index & mask, 32-byte aligned).
    .global k_rand_read
    .type k_rand_read, %function
    .balign 32
k_rand_read:
    push {r4-r7, lr}
    ldr r4, =1664525
    ldr r5, =1013904223
    mov r6, #0
1:  mla r3, r3, r4, r5
    and r7, r1, r3, lsr #8
    ldr r12, [r0, r7, lsl #2]
    add r6, r6, r12
    mla r3, r3, r4, r5
    and r7, r1, r3, lsr #8
    ldr r12, [r0, r7, lsl #2]
    add r6, r6, r12
    subs r2, r2, #2
    bhi 1b
    mov r0, r6
    pop {r4-r7, pc}
    .size k_rand_read, .-k_rand_read

@ void k_rand_write(u32* base, u32 mask, u32 stores, u32 seed)
    .global k_rand_write
    .type k_rand_write, %function
    .balign 32
k_rand_write:
    push {r4-r7, lr}
    ldr r4, =1664525
    ldr r5, =1013904223
1:  mla r3, r3, r4, r5
    and r7, r1, r3, lsr #8
    str r3, [r0, r7, lsl #2]
    mla r3, r3, r4, r5
    and r7, r1, r3, lsr #8
    str r3, [r0, r7, lsl #2]
    subs r2, r2, #2
    bhi 1b
    pop {r4-r7, pc}
    .size k_rand_write, .-k_rand_write
    .ltorg
