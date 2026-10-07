; trampoline.S -- AP startup (16-bit real -> 32-bit -> 64-bit long mode).
; Built as a flat binary (-f bin), copied to physical 0x8000 by BSP.
;
; Shared data at physical 0x9000 (u64-aligned fields):
;   [0x00] u64 page_dir      - physical PML4 address
;   [0x08] u64 stack_top     - per-AP kernel stack top (64-bit virtual)
;   [0x10] u64 ready         - AP sets to 1 when done
;   [0x18] u64 ap_id         - logical cpu id
;   [0x20] u64 gdt_packed    - bits[15:0]=limit, bits[63:16]=base>>16 (for lgdt)
;   [0x28] u64 ap_main_addr  - virtual address of ap_main()
;
; AP GDT at physical 0x8800:
;   0x00  null
;   0x08  32-bit kernel code (L=0, D=1)
;   0x10  kernel data (D/B=1)
;   0x18  64-bit kernel code (L=1, D=0)
;   0x20  kernel data

org 0x8000

BITS 16
trampoline_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; enable A20 line
    in al, 0x92
    or al, 2
    out 0x92, al

    ; load GDT from shared data at 0x9020
    ; lgdt reads 6 bytes: u16 limit + u32 base
    lgdt [0x9020]

    ; enter protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; far jump to 32-bit kernel code segment (selector 0x08)
    jmp dword 0x08:pm32

BITS 32
pm32:
    ; load kernel data segments (selector 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; load BSP's PML4 (physical address from shared data)
    mov eax, [0x9000]
    mov cr3, eax

    ; enable PAE + PGE
    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax

    ; enable long mode (EFER.LME) + NX pages (EFER.NXE): EFER is a
    ; per-CPU MSR, so every AP has to enable NXE for itself
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x900                   ; (1 << 8) LME | (1 << 11) NXE
    wrmsr

    ; enable paging + protected mode + write protect (CR0.WP): every AP
    ; must honor the page R/W bit for its own stores from the moment it
    ; turns paging on, matching the BSP (setup.asm / entry64.S).
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16) | (1 << 0)
    mov cr0, eax

    ; far jump to 64-bit kernel code segment (selector 0x18)
    jmp dword 0x18:lm64

BITS 64
lm64:
    ; load 64-bit kernel data segments (selector 0x20)
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; load kernel stack (64-bit virtual address from shared data)
    mov rax, [0x9008]
    mov rsp, rax

    ; call ap_main(u32 ap_id) via virtual address from shared data
    mov rax, [0x9028]
    mov edi, [0x9018]          ; ap_id (32-bit load, zero-extends to rdi)
    call rax

.halt:
    cli
    hlt
    jmp .halt

trampoline_end:
