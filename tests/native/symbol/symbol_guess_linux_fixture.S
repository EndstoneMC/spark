.text

.macro STRING_FUNCTION name, string
.globl \name
.hidden \name
.type \name, @function
.cfi_startproc
\name:
    leaq \string(%rip), %rax
    ret
.cfi_endproc
.size \name, .-\name
.endm

STRING_FUNCTION sparkFixtureStringTarget, spark_fixture_shared_string
STRING_FUNCTION sparkFixtureTruncatedStringTarget, spark_fixture_truncated_string
STRING_FUNCTION sparkFixtureOtherIndirectTarget, spark_fixture_other_indirect_string
STRING_FUNCTION sparkFixtureTruncatedCallStringTarget, spark_fixture_truncated_call_string
STRING_FUNCTION sparkFixtureOtherCallStringTarget, spark_fixture_other_call_string
STRING_FUNCTION sparkFixtureEmbeddedStringTarget, spark_fixture_embedded_string
STRING_FUNCTION sparkFixtureUnreachableStringTarget, spark_fixture_unreachable_string
STRING_FUNCTION sparkFixtureOverlapStringTarget, spark_fixture_overlap_string
STRING_FUNCTION sparkFixtureUnindexedStringTarget, spark_fixture_unindexed_string
STRING_FUNCTION sparkFixtureBudgetStringTarget, spark_fixture_budget_string
STRING_FUNCTION sparkFixtureLargeStringTarget, spark_fixture_large_string
STRING_FUNCTION sparkFixtureFunctionBudgetTarget, spark_fixture_function_budget_string
STRING_FUNCTION sparkFixtureBatchBudgetTarget, spark_fixture_batch_budget_string

.globl sparkFixtureMalformedStringTarget
.hidden sparkFixtureMalformedStringTarget
.type sparkFixtureMalformedStringTarget, @function
.cfi_startproc
sparkFixtureMalformedStringTarget:
    leaq spark_fixture_malformed_string(%rip), %rax
    jmp *%rax
.cfi_endproc
.size sparkFixtureMalformedStringTarget, .-sparkFixtureMalformedStringTarget

.globl sparkFixtureCallTailStringTarget
.hidden sparkFixtureCallTailStringTarget
.type sparkFixtureCallTailStringTarget, @function
.cfi_startproc
sparkFixtureCallTailStringTarget:
    leaq spark_fixture_call_tail_string(%rip), %rax
    call .L_call_tail_callee
.cfi_endproc
.size sparkFixtureCallTailStringTarget, .-sparkFixtureCallTailStringTarget
.L_call_tail_callee:
    ret

.globl sparkFixtureTruncatedCallBytes
.hidden sparkFixtureTruncatedCallBytes
.type sparkFixtureTruncatedCallBytes, @function
.cfi_startproc
sparkFixtureTruncatedCallBytes:
    leaq spark_fixture_truncated_call_string(%rip), %rax
    .byte 0xe8, 0x00, 0x00
.cfi_endproc
.size sparkFixtureTruncatedCallBytes, .-sparkFixtureTruncatedCallBytes

.globl sparkFixtureOtherCallFake
.hidden sparkFixtureOtherCallFake
.type sparkFixtureOtherCallFake, @function
.cfi_startproc
sparkFixtureOtherCallFake:
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_other_call_string - . - 4
    .byte 0x90
    call .L_other_call_callee
.cfi_endproc
.size sparkFixtureOtherCallFake, .-sparkFixtureOtherCallFake
.L_other_call_callee:
    ret

.globl sparkFixtureSyscallStringTarget
.hidden sparkFixtureSyscallStringTarget
.type sparkFixtureSyscallStringTarget, @function
.cfi_startproc
sparkFixtureSyscallStringTarget:
    leaq spark_fixture_syscall_string(%rip), %rax
    syscall
    ret
.cfi_endproc
.size sparkFixtureSyscallStringTarget, .-sparkFixtureSyscallStringTarget

.globl sparkFixtureOtherIndirectFake
.hidden sparkFixtureOtherIndirectFake
.type sparkFixtureOtherIndirectFake, @function
.cfi_startproc
sparkFixtureOtherIndirectFake:
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_other_indirect_string - . - 4
    .byte 0x90
    jmp *%rax
.cfi_endproc
.size sparkFixtureOtherIndirectFake, .-sparkFixtureOtherIndirectFake

.globl sparkFixtureTruncatedBytes
.hidden sparkFixtureTruncatedBytes
.type sparkFixtureTruncatedBytes, @function
.cfi_startproc
sparkFixtureTruncatedBytes:
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_truncated_string - . - 4
.cfi_endproc
.size sparkFixtureTruncatedBytes, .-sparkFixtureTruncatedBytes

.globl sparkFixturePrefixedStringTarget
.hidden sparkFixturePrefixedStringTarget
.type sparkFixturePrefixedStringTarget, @function
.cfi_startproc
sparkFixturePrefixedStringTarget:
    .byte 0x66, 0x48, 0x8d, 0x05
    .long spark_fixture_shared_string - . - 4
    ret
.cfi_endproc
.size sparkFixturePrefixedStringTarget, .-sparkFixturePrefixedStringTarget

// Shared false hits follow the second validated owner and are terminal skips.
.rept 2048
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_shared_string - . - 4
    .byte 0xc3
.endr

STRING_FUNCTION sparkFixtureUniqueStringTarget, spark_fixture_unique_string

.globl sparkFixtureWeakTarget
.hidden sparkFixtureWeakTarget
.type sparkFixtureWeakTarget, @function
.cfi_startproc
sparkFixtureWeakTarget:
    leaq spark_fixture_weak_a(%rip), %rax
    leaq spark_fixture_weak_b(%rip), %rax
    leaq spark_fixture_weak_c(%rip), %rax
    ret
.cfi_endproc
.size sparkFixtureWeakTarget, .-sparkFixtureWeakTarget

.globl sparkFixtureWeakSharedTarget
.hidden sparkFixtureWeakSharedTarget
.type sparkFixtureWeakSharedTarget, @function
.cfi_startproc
sparkFixtureWeakSharedTarget:
    leaq spark_fixture_weak_shared_a(%rip), %rax
    leaq spark_fixture_weak_shared_b(%rip), %rax
    leaq spark_fixture_weak_shared_c(%rip), %rax
    ret
.cfi_endproc
.size sparkFixtureWeakSharedTarget, .-sparkFixtureWeakSharedTarget

.globl sparkFixtureWeakOtherTarget
.hidden sparkFixtureWeakOtherTarget
.type sparkFixtureWeakOtherTarget, @function
.cfi_startproc
sparkFixtureWeakOtherTarget:
    leaq spark_fixture_weak_shared_a(%rip), %rax
    ret
.cfi_endproc
.size sparkFixtureWeakOtherTarget, .-sparkFixtureWeakOtherTarget

.globl sparkFixtureWeakAmbiguousTarget
.hidden sparkFixtureWeakAmbiguousTarget
.type sparkFixtureWeakAmbiguousTarget, @function
.cfi_startproc
sparkFixtureWeakAmbiguousTarget:
    leaq spark_fixture_weak_ambiguous_a(%rip), %rax
    leaq spark_fixture_weak_ambiguous_b(%rip), %rax
    leaq spark_fixture_weak_ambiguous_c(%rip), %rax
    ret
.cfi_endproc
.size sparkFixtureWeakAmbiguousTarget, .-sparkFixtureWeakAmbiguousTarget

.globl sparkFixtureInteriorThunk
.hidden sparkFixtureInteriorThunk
.type sparkFixtureInteriorThunk, @function
.cfi_startproc
sparkFixtureInteriorThunk:
    jmp sparkFixtureUniqueStringTarget + 1
.cfi_endproc
.size sparkFixtureInteriorThunk, .-sparkFixtureInteriorThunk

.globl sparkFixtureEmbeddedBytes
.hidden sparkFixtureEmbeddedBytes
.type sparkFixtureEmbeddedBytes, @function
.cfi_startproc
sparkFixtureEmbeddedBytes:
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_embedded_string - . - 4
    .byte 0x90
    ret
.cfi_endproc
.size sparkFixtureEmbeddedBytes, .-sparkFixtureEmbeddedBytes

.globl sparkFixtureUnreachableBytes
.hidden sparkFixtureUnreachableBytes
.type sparkFixtureUnreachableBytes, @function
.cfi_startproc
sparkFixtureUnreachableBytes:
    jmp .L_unreachable_ret
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_unreachable_string - . - 4
.L_unreachable_ret:
    ret
.cfi_endproc
.size sparkFixtureUnreachableBytes, .-sparkFixtureUnreachableBytes

.globl sparkFixtureOverlapBytes
.hidden sparkFixtureOverlapBytes
.type sparkFixtureOverlapBytes, @function
.cfi_startproc
sparkFixtureOverlapBytes:
    .byte 0x48, 0xb8
.L_overlap_raw:
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_overlap_string - . - 4
    .byte 0x90
    je .L_overlap_raw
    ret
.cfi_endproc
.size sparkFixtureOverlapBytes, .-sparkFixtureOverlapBytes

sparkFixtureLargeBytes:
.cfi_startproc
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_large_string - . - 4
    .byte 0x90
    .rept 65540
    nop
    .endr
    ret
.cfi_endproc

sparkFixtureFunctionBudgetBytes:
.cfi_startproc
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_function_budget_string - . - 4
    .byte 0x90
    .rept 8200
    nop
    .endr
    ret
.cfi_endproc

// Each FDE is a separate small function so the batch validation cache reaches
// its fixed 4096-record admission bound without retaining large byte maps.
.altmacro
.set spark_budget_index, 0
.rept 4097
.cfi_startproc
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_budget_string - . - 4
    .byte 0x90
    ret
.cfi_endproc
.set spark_budget_index, spark_budget_index + 1
.endr
.noaltmacro

// The larger set reaches the one-million-instruction batch budget.
.rept 4097
.cfi_startproc
    .byte 0x48, 0xb8, 0x48, 0x8d, 0x05
    .long spark_fixture_batch_budget_string - . - 4
    .byte 0x90
    .rept 255
    nop
    .endr
    ret
.cfi_endproc
.endr

// These executable bytes are intentionally outside every FDE.
.type spark_fixture_unindexed_bytes, @object
spark_fixture_unindexed_bytes:
.rept 2048
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_unindexed_string - . - 4
    .byte 0xc3
.endr
spark_fixture_unindexed_weak_bytes:
    .byte 0x48, 0x8d, 0x05
    .long spark_fixture_weak_ambiguous_a - . - 4
    .byte 0xc3

STRING_FUNCTION sparkFixtureLateStringTarget, spark_fixture_late_string

.section .rodata
.type spark_fixture_shared_string, @object
.size spark_fixture_shared_string, 20
spark_fixture_shared_string:
    .asciz "Level - tick shared"
.type spark_fixture_truncated_string, @object
.size spark_fixture_truncated_string, 23
spark_fixture_truncated_string:
    .asciz "Level - tick truncated"
.type spark_fixture_other_indirect_string, @object
.size spark_fixture_other_indirect_string, 28
spark_fixture_other_indirect_string:
    .asciz "Level - tick other indirect"
.type spark_fixture_call_tail_string, @object
.size spark_fixture_call_tail_string, 23
spark_fixture_call_tail_string:
    .asciz "Level - tick call tail"
.type spark_fixture_truncated_call_string, @object
.size spark_fixture_truncated_call_string, 28
spark_fixture_truncated_call_string:
    .asciz "Level - tick truncated call"
.type spark_fixture_other_call_string, @object
.size spark_fixture_other_call_string, 24
spark_fixture_other_call_string:
    .asciz "Level - tick other call"
.type spark_fixture_syscall_string, @object
.size spark_fixture_syscall_string, 21
spark_fixture_syscall_string:
    .asciz "Level - tick syscall"
.type spark_fixture_unique_string, @object
.size spark_fixture_unique_string, 20
spark_fixture_unique_string:
    .asciz "Level - tick unique"
.type spark_fixture_malformed_string, @object
.size spark_fixture_malformed_string, 23
spark_fixture_malformed_string:
    .asciz "Level - tick malformed"
.type spark_fixture_embedded_string, @object
.size spark_fixture_embedded_string, 22
spark_fixture_embedded_string:
    .asciz "Level - tick embedded"
.type spark_fixture_unreachable_string, @object
.size spark_fixture_unreachable_string, 25
spark_fixture_unreachable_string:
    .asciz "Level - tick unreachable"
.type spark_fixture_overlap_string, @object
.size spark_fixture_overlap_string, 21
spark_fixture_overlap_string:
    .asciz "Level - tick overlap"
.type spark_fixture_unindexed_string, @object
.size spark_fixture_unindexed_string, 23
spark_fixture_unindexed_string:
    .asciz "Level - tick unindexed"
.type spark_fixture_budget_string, @object
.size spark_fixture_budget_string, 20
spark_fixture_budget_string:
    .asciz "Level - tick budget"
.type spark_fixture_large_string, @object
.size spark_fixture_large_string, 19
spark_fixture_large_string:
    .asciz "Level - tick large"
.type spark_fixture_function_budget_string, @object
.size spark_fixture_function_budget_string, 29
spark_fixture_function_budget_string:
    .asciz "Level - tick function budget"
.type spark_fixture_batch_budget_string, @object
.size spark_fixture_batch_budget_string, 26
spark_fixture_batch_budget_string:
    .asciz "Level - tick batch budget"
.type spark_fixture_late_string, @object
.size spark_fixture_late_string, 18
spark_fixture_late_string:
    .asciz "Level - tick late"

.type spark_fixture_weak_a, @object
.size spark_fixture_weak_a, 11
spark_fixture_weak_a:
    .asciz "alpha beta"
.type spark_fixture_weak_b, @object
.size spark_fixture_weak_b, 12
spark_fixture_weak_b:
    .asciz "gamma delta"
.type spark_fixture_weak_c, @object
.size spark_fixture_weak_c, 13
spark_fixture_weak_c:
    .asciz "epsilon zeta"
.type spark_fixture_weak_shared_a, @object
.size spark_fixture_weak_shared_a, 11
spark_fixture_weak_shared_a:
    .asciz "shared one"
.type spark_fixture_weak_shared_b, @object
.size spark_fixture_weak_shared_b, 11
spark_fixture_weak_shared_b:
    .asciz "shared two"
.type spark_fixture_weak_shared_c, @object
.size spark_fixture_weak_shared_c, 13
spark_fixture_weak_shared_c:
    .asciz "shared three"
.type spark_fixture_weak_ambiguous_a, @object
.size spark_fixture_weak_ambiguous_a, 14
spark_fixture_weak_ambiguous_a:
    .asciz "uncertain one"
.type spark_fixture_weak_ambiguous_b, @object
.size spark_fixture_weak_ambiguous_b, 14
spark_fixture_weak_ambiguous_b:
    .asciz "uncertain two"
.type spark_fixture_weak_ambiguous_c, @object
.size spark_fixture_weak_ambiguous_c, 16
spark_fixture_weak_ambiguous_c:
    .asciz "uncertain three"

.section .note.GNU-stack,"",@progbits
