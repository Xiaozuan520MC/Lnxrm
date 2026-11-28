; SPDX-License-Identifier: Apache-2.0
; lnxrm -- gzip decompression stub entry
;
; Linked at physical 0x100000 by arch/decompress.ld, together with the
; inflate blob (kernel/lib/inflate.c, linked @0x200000, converted to a
; raw binary and embedded via incbin).
;
; Entered from arch/setup.asm after it has switched to long mode and built
; the temporary identity page tables (so 0x100000..0x400000 are directly
; addressable).
;
; bzImage payload layout (loaded at 0x100000 by setup.bin):
;   [0x100000,             _decompress_end)   this stub + embedded inflate
;                                              blob data
;   [_decompress_end,      _decompress_end+compressed_size)  gzip kernel
;
; Safe execution zones (output -> 0x100000 never reaches these):
;   0x200000: inflate code (copied here by this stub, then run from here)
;   0x300000: gzip data  (copied here, then inflated back to 0x100000)
;
; The stub:
;   1. Copies its embedded inflate blob to 0x200000
;   2. Copies the gzip data to 0x300000
;   3. Jumps to inflate_main() at 0x200000, which inflates to 0x100000
;      and then jumps to the kernel's 64-bit entry (_start64's physical
;      address, baked into kernel_entry_addr).
;
; Symbols (defined by arch/decompress.ld / linker / Makefile):
;   _decompress_end      -- end of stub (= start of gzip data in payload)
;   compressed_size      -- size of gzip data in bytes (baked at build time)
;   inflate_main         -- entry of the inflate blob (0x200000)

BITS 64

section .text
global decompress_entry

extern _decompress_end
extern compressed_size
extern inflate_main_addr
extern kernel_entry_addr

section .rodata
align 8
; Embedded inflate binary (built by arch/inflate.ld, linked at 0x200000).
incbin_bin:
    incbin "build/inflate.bin"
incbin_bin_end:

inflate_bin_data:
    dq incbin_bin
inflate_bin_size:
    dq incbin_bin_end - incbin_bin

; ---- minimal COM1 (0x3F8) putchar for early debug ----------------------
COM1 equ 0x3F8

section .text
decompress_entry:
    cli
    cld

    ; Temporary stack at 4 MiB.  The SysV AMD64 ABI requires rsp % 16 == 8
    ; at function entry (call has just pushed the return address), so leave
    ; the stack 8 bytes below a 16-byte boundary before jmp'ing into C.
    mov rsp, 0x400000
    sub rsp, 8

    ; serial debug marker: 'D'
    mov dx, COM1 + 5             ; LSR
.waitD:
    in   al, dx
    test al, 0x20                ; THRE ready?
    jz   .waitD
    mov dx, COM1                 ; THR
    mov al, 'D'
    out  dx, al

    ; ---- 1. Copy inflate blob to 0x200000 -----------------------------
    mov rsi, [inflate_bin_data]   ; source: address of embedded inflate blob
    mov rdx, [inflate_bin_size]   ; rdx = inflate blob size
    mov rdi, 0x200000           ; destination
    mov rcx, rdx
    shr rcx, 3
    rep movsq
    mov rcx, rdx
    and rcx, 7
    rep movsb

    ; serial debug marker: '1'
    mov dx, COM1 + 5
.wait1:
    in   al, dx
    test al, 0x20
    jz   .wait1
    mov dx, COM1
    mov al, '1'
    out  dx, al

    ; ---- 2. Copy gzip data to 0x300000 --------------------------------
    ; rsi = gzip data source = _decompress_end (start of gzip data)
    mov rsi, _decompress_end

    ; rdx = compressed size (baked at build time)
    mov rdx, [compressed_size]

    mov rdi, 0x300000           ; destination
    mov rcx, rdx
    shr rcx, 3
    rep movsq
    mov rcx, rdx
    and rcx, 7
    rep movsb

    ; serial debug marker: '2'
    mov dx, COM1 + 5
.wait2:
    in   al, dx
    test al, 0x20
    jz   .wait2
    mov dx, COM1
    mov al, '2'
    out  dx, al

    ; ---- 3. Jump to inflate_main at 0x200000 -------------------------
    ; System V AMD64 ABI:
    ;   rdi = 0x300000  (compressed gzip data)
    ;   rsi = 0x100000  (output destination)
    ;   rdx = compressed size
    ;   rcx = kernel 64-bit entry (_start64's physical address, NOT 0x100000:
    ;         that is _start32, 32-bit code that faults under a 64-bit CS)
    mov rdi, 0x300000
    mov rsi, 0x100000

    ; serial debug marker: 'I'
    mov dx, COM1 + 5
.waitI:
    in   al, dx
    test al, 0x20
    jz   .waitI
    mov dx, COM1
    mov al, 'I'
    out  dx, al

    ; Load the size LAST: the serial debug above writes dx (low 16 bits of
    ; rdx) for the COM1 port, which would otherwise corrupt src_len.
    mov rdx, [compressed_size]
    mov rcx, [kernel_entry_addr]

    mov rax, [inflate_main_addr]
    jmp rax
