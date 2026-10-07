; usr/crt0.S -- user program entry.
; The kernel lays the stack out as the SysV AMD64 process ABI requires:
;   [rsp]      argc
;   [rsp+8]    argv[0 .. argc-1], NULL
;              envp[0 .. envc-1], NULL
;              AT_NULL auxv pair
BITS 64
global _start
global environ
extern main

section .text
_start:
    xor ebp, ebp
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    mov rcx, rdi
    lea rdx, [rsi + rcx * 8 + 8] ; envp == &argv[argc + 1]
    mov [rel environ], rdx      ; publish for the program
    call main
    mov rdi, rax                ; exit code
    mov eax, 12                 ; SYS_exit
    int 0x80
.hang:
    jmp .hang

section .bss
align 8
environ: resq 1
