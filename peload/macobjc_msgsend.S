/* objc_msgSend and friends.
 *
 * These cannot be written in C. A message send passes the receiver and selector
 * in the first two argument registers and everything else straight through, then
 * tail-jumps to the implementation -- so the trampoline must not disturb any
 * argument register, including the vector ones and the variadic count in %al.
 *
 * System V AMD64: integer args in rdi rsi rdx rcx r8 r9, floating point in
 * xmm0-7, and %al holds the number of vector registers used for a variadic call.
 * All of those are saved, the lookup is called, and all are restored before the
 * jump, so the implementation sees exactly what the caller sent.
 *
 * The resolved implementation is kept in the frame, not in %r11. %r11 is
 * caller-saved *and* the scratch register the lazy PLT resolver uses, so any
 * further call -- macobjc_super_receiver below is one -- may clobber it, and the
 * tail jump then goes somewhere arbitrary. Loading it back at the last moment
 * costs nothing and removes the hazard.
 *
 * Frame: 0xc8 bytes. On entry the call has left rsp 8 mod 16, and 0xb8 is 8 mod
 * 16, so rsp is 16-byte aligned inside -- which both the ABI and the movaps
 * saves below require.
 */
    .text

#define FRAME 0xc8
#define IMPSLOT 0x38

/* Save the argument registers into the frame. */
.macro SAVE_ARGS
    movq    %rdi, 0x00(%rsp)
    movq    %rsi, 0x08(%rsp)
    movq    %rdx, 0x10(%rsp)
    movq    %rcx, 0x18(%rsp)
    movq    %r8,  0x20(%rsp)
    movq    %r9,  0x28(%rsp)
    movq    %rax, 0x30(%rsp)          /* variadic vector-register count */
    movaps  %xmm0, 0x40(%rsp)
    movaps  %xmm1, 0x50(%rsp)
    movaps  %xmm2, 0x60(%rsp)
    movaps  %xmm3, 0x70(%rsp)
    movaps  %xmm4, 0x80(%rsp)
    movaps  %xmm5, 0x90(%rsp)
    movaps  %xmm6, 0xa0(%rsp)
    movaps  %xmm7, 0xb0(%rsp)
.endm

.macro RESTORE_ARGS
    movaps  0x40(%rsp), %xmm0
    movaps  0x50(%rsp), %xmm1
    movaps  0x60(%rsp), %xmm2
    movaps  0x70(%rsp), %xmm3
    movaps  0x80(%rsp), %xmm4
    movaps  0x90(%rsp), %xmm5
    movaps  0xa0(%rsp), %xmm6
    movaps  0xb0(%rsp), %xmm7
    movq    0x30(%rsp), %rax
    movq    0x00(%rsp), %rdi
    movq    0x08(%rsp), %rsi
    movq    0x10(%rsp), %rdx
    movq    0x18(%rsp), %rcx
    movq    0x20(%rsp), %r8
    movq    0x28(%rsp), %r9
.endm

/* id objc_msgSend(id self, SEL sel, ...) */
    .globl  macobjc_msgSend
    .type   macobjc_msgSend, @function
macobjc_msgSend:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x00(%rsp), %rdi          /* self */
    movq    0x08(%rsp), %rsi          /* sel  */
    call    macobjc_lookup@PLT
    movq    %rax, IMPSLOT(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSend, .-macobjc_msgSend

/* void objc_msgSend_stret(void *ret, id self, SEL sel, ...)
 * A struct return shifts everything along one: the hidden pointer takes rdi, so
 * the receiver is in rsi and the selector in rdx. */
    .globl  macobjc_msgSend_stret
    .type   macobjc_msgSend_stret, @function
macobjc_msgSend_stret:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x08(%rsp), %rdi          /* self */
    movq    0x10(%rsp), %rsi          /* sel  */
    call    macobjc_lookup@PLT
    movq    %rax, IMPSLOT(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSend_stret, .-macobjc_msgSend_stret

/* id objc_msgSendSuper2(struct objc_super2 *s, SEL sel, ...)
 * The lookup starts above s->cls, and the implementation must be called with
 * s->receiver in rdi -- not the super struct. */
    .globl  macobjc_msgSendSuper2
    .type   macobjc_msgSendSuper2, @function
macobjc_msgSendSuper2:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x00(%rsp), %rdi          /* the objc_super2 */
    movq    0x08(%rsp), %rsi          /* sel */
    call    macobjc_lookup_super@PLT
    movq    %rax, IMPSLOT(%rsp)
    movq    0x00(%rsp), %rdi
    call    macobjc_super_receiver@PLT
    movq    %rax, 0x00(%rsp)          /* replace the super struct with the receiver */
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSendSuper2, .-macobjc_msgSendSuper2

/* void objc_msgSendSuper2_stret(void *ret, struct objc_super2 *s, SEL sel, ...) */
    .globl  macobjc_msgSendSuper2_stret
    .type   macobjc_msgSendSuper2_stret, @function
macobjc_msgSendSuper2_stret:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x08(%rsp), %rdi
    movq    0x10(%rsp), %rsi
    call    macobjc_lookup_super@PLT
    movq    %rax, IMPSLOT(%rsp)
    movq    0x08(%rsp), %rdi
    call    macobjc_super_receiver@PLT
    movq    %rax, 0x08(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSendSuper2_stret, .-macobjc_msgSendSuper2_stret

/* The implementation used when a class does not respond to a selector.
 *
 * Objective-C guarantees such a send evaluates to nil, and "nil" means the
 * return registers must actually be zero. A C function returning void does not
 * do that -- it leaves whatever was in %rax, which here is the address of this
 * very function, and the caller then treats that as an object and dereferences
 * it. Zeroing rax, rdx and the two vector return registers covers integer,
 * pointer, 128-bit and floating-point returns alike.
 */
/* The legacy "fixup" dispatch ABI.
 *
 * An older Mach-O does not put the selector in %rsi. It emits, per call site, a
 * `struct message_ref { IMP imp; SEL sel; }` in __DATA, loads its *address* into
 * %rsi, and calls through `ref->imp` -- which the linker initialises to
 * objc_msgSend_fixup. So the real selector is at 8(%rsi), and the receiver is in
 * %rdi as usual.
 *
 * Ragnarok is built this way. Without these, every Objective-C call it makes went
 * to whatever the unresolved import happened to be bound to, its PNG loader
 * silently returned nothing, and the crash surfaced two frames later as a virtual
 * call on a null bitmap. Nothing in the trace showed a message being sent, because
 * as far as the runtime was concerned none ever was.
 */
    .globl  macobjc_msgSend_fixup
    .type   macobjc_msgSend_fixup, @function
macobjc_msgSend_fixup:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x00(%rsp), %rdi          /* self                        */
    movq    0x08(%rsp), %rsi          /* &message_ref                */
    movq    0x08(%rsi), %rsi          /* ref->sel                    */
    movq    %rsi, 0x08(%rsp)          /* hand the callee a real SEL  */
    call    macobjc_lookup@PLT
    movq    %rax, IMPSLOT(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSend_fixup, .-macobjc_msgSend_fixup

/* Struct return: the hidden pointer takes %rdi, so self is %rsi and the
 * message_ref is %rdx. */
    .globl  macobjc_msgSend_stret_fixup
    .type   macobjc_msgSend_stret_fixup, @function
macobjc_msgSend_stret_fixup:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x08(%rsp), %rdi          /* self         */
    movq    0x10(%rsp), %rsi          /* &message_ref */
    movq    0x08(%rsi), %rsi          /* ref->sel     */
    movq    %rsi, 0x10(%rsp)
    call    macobjc_lookup@PLT
    movq    %rax, IMPSLOT(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSend_stret_fixup, .-macobjc_msgSend_stret_fixup

/* super2, fixup flavour: %rdi is the {receiver, class} pair, %rsi the ref. */
    .globl  macobjc_msgSendSuper2_fixup
    .type   macobjc_msgSendSuper2_fixup, @function
macobjc_msgSendSuper2_fixup:
    subq    $FRAME, %rsp
    SAVE_ARGS
    movq    0x00(%rsp), %rdi          /* &super2      */
    movq    0x08(%rsp), %rsi          /* &message_ref */
    movq    0x08(%rsi), %rsi          /* ref->sel     */
    movq    %rsi, 0x08(%rsp)
    call    macobjc_lookup_super@PLT
    movq    %rax, IMPSLOT(%rsp)
    /* super2 also rewrites the receiver: the callee must see the object, not
     * the pair. */
    movq    0x00(%rsp), %rdi
    call    macobjc_super_receiver@PLT
    movq    %rax, 0x00(%rsp)
    RESTORE_ARGS
    movq    IMPSLOT(%rsp), %r11
    addq    $FRAME, %rsp
    jmp     *%r11
    .size   macobjc_msgSendSuper2_fixup, .-macobjc_msgSendSuper2_fixup

    .globl  macobjc_nil_return
    .type   macobjc_nil_return, @function
macobjc_nil_return:
    xorq    %rax, %rax
    xorq    %rdx, %rdx
    xorps   %xmm0, %xmm0
    xorps   %xmm1, %xmm1
    ret
    .size   macobjc_nil_return, .-macobjc_nil_return

    .section .note.GNU-stack,"",@progbits
