; SPDX-License-Identifier: Apache-2.0

; ld.so entry (userland/ldso/entry.S).
;
; The kernel jumps here exactly as it would any other program: argc, argv,
; envp and the auxiliary vector on the stack, 16-byte aligned.  crt0 is
; deliberately NOT used -- this image cannot depend on anything it has not
; loaded itself, so it parses the stack it was handed rather than sharing
; the startup file with the programs it is about to start.
;
; The one subtlety is the hand-off.  The program's own _start reads argc
; from [rsp], so ld.so cannot `call` it: that would leave a return address
; where argc should be.  The stack the kernel built is saved here, and
; restored before jumping, so the program sees precisely the frame execve
; constructed -- including the auxiliary vector, which crt0 walks past to
; publish `environ`.
BITS 64
global _start
extern ldso_main

section .text
_start:
    mov [rel saved_rsp], rsp      ; the frame execve built, for the hand-off
    xor ebp, ebp
    mov rdi, [rsp]                ; argc
    lea rsi, [rsp + 8]            ; argv
    mov rcx, rdi
    lea rdx, [rsi + rcx * 8 + 8]  ; envp == &argv[argc + 1]

    ; envp runs to its NULL terminator; the auxiliary vector begins at the
    ; word after it.  Walk it here rather than in C so that ldso_main can
    ; be an ordinary function with ordinary arguments.
    mov r8, rdx
.walk:
    cmp qword [r8], 0
    je .walked
    add r8, 8
    jmp .walk
.walked:
    add r8, 8                    ; step past the NULL
    mov rcx, r8                  ; 4th argument: auxv

    call ldso_main               ; returns the program's entry point

    mov rsp, [rel saved_rsp]     ; give the program the stack it was given
    xor ebp, ebp
    jmp rax                      ; no return address: its _start owns the frame

section .bss
align 8
saved_rsp: resq 1
