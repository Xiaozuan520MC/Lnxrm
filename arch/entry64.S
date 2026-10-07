; lnxrm -- 64-bit kernel entry, interrupt stubs, context switch
; Assembled with nasm -f elf64. Linked at VMA 0xffffffff80100000 (LMA 0x100000).

%define PML4_M    0x50000            ; master PML4 (permanent)
%define PDPT_M    0x51000            ; identity subtree PDPT
%define PD_M      0x52000            ; identity 1 GiB, 2 MiB pages
%define PDPT_HI   0x53000            ; high-half subtree PDPT (PML4[511])
%define PD_HI     0x54000            ; kernel/heap/dev/vga, 2 MiB pages
%define PT_VGA    0x55000            ; text framebuffer window
%define PT_DEV    0x56000            ; device MMIO window

BITS 64

section .text.prologue
global _start64
global _start32
extern start_kernel
extern __bss_start
extern __bss_end
extern __boot_stack_top

global pd_hi                          ; C fills heap entries pre-switch
global pt_dev                         ; C fills AHCI ABAR mapping

; ==========================================================================
; 32-bit protected-mode entry for GRUB's `linux` command.
;
; GRUB jumps here with:
;   CS = 32-bit flat code, DS/ES/SS = flat data
;   CR0.PE = 1, CR0.PG = 0, CR4.PAE = 0, EFER.LME = 0
;   ESI    = physical address of boot_params (zero page)
;   ESP    = some GRUB-provided stack
;
; We:
;   1. stash ESI (boot_params) at physical 0x6F00
;   2. build temporary page tables at 0x60000 (same layout as setup.asm)
;   3. enable PAE + long mode
;   4. jump to _start64's physical alias (NOT 0x100000 -- _start64 is
;      linked after _start32 in the same section)
;
; NOTE: this code runs at physical 0x100000 but is linked at the high VMA.
;       All absolute symbol references must be computed at runtime.
; ==========================================================================

BITS 32

_start32:
    cli
    cld

    ; --- stash boot_params (ESI) at physical 0x6F00 ----------------------
    mov  [0x6F00], esi

    ; --- load our temporary GDT ------------------------------------------
    ; We're linked at high VMA (0xffffffff80100000) but running at
    ; physical 0x100000.  Compute the runtime address of gdt32_desc.
    call .get_base
.get_base:
    pop  ebx
    ; ebx = runtime address of .get_base
    ; (.get_base - _start32) is a link-time offset; since the whole
    ; payload is copied verbatim, it equals the runtime offset too.
    sub  ebx, (.get_base - _start32)     ; ebx = runtime addr of _start32
    lea  eax, [ebx + (gdt32_desc - _start32)]
    lgdt [eax]

    ; --- CR4.PAE ---------------------------------------------------------
    mov  eax, cr4
    or   eax, 0x20
    mov  cr4, eax

    ; --- temporary page tables at 0x60000 --------------------------------
    ; pml4[0]   -> pdpt 0x61000
    ; pml4[511] -> pdpt 0x61000
    ; pdpt[0]   -> pd   0x62000 (identity low 1 GiB)
    ; pdpt[510] -> pd   0x63000 (high half, kernel window)
    mov  DWORD [0x60000 + 0*8],   0x61003
    mov  DWORD [0x60000 + 511*8], 0x61003
    mov  DWORD [0x61000 + 0*8],   0x62003
    mov  DWORD [0x61000 + 510*8], 0x63003

    ; identity map low 1 GiB (2 MiB pages)
    mov  edi, 0x62000
    mov  eax, 0x83
    mov  ecx, 512
.id32:
    mov  [edi], eax
    mov  DWORD [edi+4], 0
    add  eax, 0x200000
    add  edi, 8
    loop .id32

    ; high half: kernel first 32 x 2 MiB
    mov  edi, 0x63000
    mov  eax, 0x83
    mov  ecx, 32
.hi32:
    mov  [edi], eax
    mov  DWORD [edi+4], 0
    add  eax, 0x200000
    add  edi, 8
    loop .hi32
    ; zero remaining
    mov  ecx, 480
.z32:
    mov  DWORD [edi], 0
    mov  DWORD [edi+4], 0
    add  edi, 8
    loop .z32

    ; VGA text window: PD_HI[96] -> PT 0x64000 -> phys 0xB8000
    mov  DWORD [0x63000 + 96*8], 0x64003
    mov  DWORD [0x64000], 0xB8003

    ; --- CR3 -------------------------------------------------------------
    mov  eax, 0x60000
    mov  cr3, eax

    ; --- EFER.LME + NXE --------------------------------------------------
    ; NXE (bit 11) makes bit 63 of a PTE mean "no execute"; user pages
    ; are mapped with it, so it must be on before any of them exists.
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 0x900
    wrmsr

    ; --- CR0.PG + CR0.WP --------------------------------------------------
    ; WP (bit 16) makes every ring-0 store honor the page R/W bit: a kernel
    ; write to a read-only page (#PF) can no longer corrupt it.  Every page
    ; mapped so far is P|W, so switching it on here cannot fault.
    mov  eax, cr0
    or   eax, 0x80010000
    mov  cr0, eax

    ; --- jump to _start64's physical alias -------------------------------
    ; GDT[3] = 64-bit code (selector 0x18).
    ;
    ; _start64 is linked AFTER _start32 in the same section, so its
    ; physical address is 0x100000 + (_start64 - _start32).  Do NOT jump
    ; to 0x100000 -- that is _start32 itself, and executing 32-bit code
    ; under a 64-bit CS triggers #UD.
    jmp  0x18:(_start64 - _start32 + 0x100000)

; --- temporary GDT for the 32-bit stub -----------------------------------
align 8
gdt32:
    dq 0
    dq 0x00CF9A000000FFFF      ; 0x08 32-bit code
    dq 0x00CF92000000FFFF      ; 0x10 data
    dq 0x00209A0000000000      ; 0x18 64-bit code
gdt32_desc:
    dw gdt32_desc - gdt32 - 1
    dd gdt32 - _start32 + 0x100000

; ==========================================================================
; 64-bit entry (both QEMU -kernel and GRUB land here eventually).
;
; Entered from:
;   - setup.asm (QEMU -kernel) after it built page tables and switched
;     to long mode.  EBX = boot_params from setup.asm (may be 0).
;   - _start32 (GRUB) after it built page tables and switched to long
;     mode.  0x6F00 holds the GRUB boot_params pointer.
; ==========================================================================

BITS 64
_start64:
    mov al, 'K'                     ; debugcon marker
    out 0xE9, al
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; ---- master page tables ----------------------------------------------
    xor ecx, ecx
.clr_pml4:
    mov DWORD [PML4_M + rcx*8], 0
    mov DWORD [PML4_M + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pml4

    mov QWORD [PML4_M + 0*8],   PDPT_M | 0x003
    mov QWORD [PML4_M + 511*8], PDPT_HI | 0x003

    xor ecx, ecx
.clr_pdptm:
    mov DWORD [PDPT_M + rcx*8], 0
    mov DWORD [PDPT_M + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pdptm
    mov DWORD [PDPT_M + 0*8], PD_M | 0x003

    xor ecx, ecx
.clr_pdpthi:
    mov DWORD [PDPT_HI + rcx*8], 0
    mov DWORD [PDPT_HI + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pdpthi
    mov DWORD [PDPT_HI + 510*8], PD_HI | 0x003

    mov al, 'b'
    out 0xE9, al
    ; identity map low 1 GiB (2 MiB pages)
    mov rdi, PD_M
    mov eax, 0x083
.idloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    cmp eax, 0x083 + 512*0x200000
    jb .idloop

    ; high half: kernel image window 0..32 x 2 MiB
    mov rdi, PD_HI
    mov eax, 0x083
.hiloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    cmp eax, 0x083 + 32*0x200000
    jb .hiloop
.zhi:                                   ; zero entries 32..511
    mov DWORD [edi], 0
    mov DWORD [edi+4], 0
    add edi, 8
    cmp edi, PD_HI + 512*8
    jb .zhi

    mov al, 'c'
    out 0xE9, al
    ; VGA window 0xffffffffb8000000 -> PD_HI[96] -> PT_VGA[0] = 0xB8000
    mov DWORD [PD_HI + 96*8], PT_VGA | 0x003
    mov rdi, PT_VGA
    xor ecx, ecx
.zvga:
    mov DWORD [rdi + rcx*8], 0
    mov DWORD [rdi + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .zvga
    mov DWORD [PT_VGA], 0xB8003

    ; device MMIO window -> PD_HI[128] -> PT_DEV (C fills in the PTE)
    mov DWORD [PD_HI + 128*8], PT_DEV | 0x003
    mov rdi, PT_DEV
    xor ecx, ecx
.zdev:
    mov DWORD [rdi + rcx*8], 0
    mov DWORD [rdi + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .zdev

.activate:
    mov al, 'd'
    out 0xE9, al
    mov rax, cr4
    or rax, 0x20                        ; PAE only
    mov cr4, rax
    mov al, 'f'
    out 0xE9, al
    mov rax, PML4_M
    mov cr3, rax                   ; switch to master tables
    mov al, 'e'
    out 0xE9, al
    ; ---- clear .bss ----
    movabs rdi, __bss_start
    movabs rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3
    xor eax, eax
    rep stosq

    ; ---- boot stack, jump into C at the HIGH address ----
    mov al, 'M'
    out 0xE9, al
    movabs rsp, __boot_stack_top
    xor ebp, ebp

    ; ---- boot_params: the GRUB path stashed it at 0x6F00 (see _start32);
    ;      main.c reads that sentinel itself, so no argument is passed.
    movabs rax, start_kernel
    call rax
.stop:
    cli
    hlt
    jmp .stop

; --------------------------------------------------------------------------
section .text

; void gdt_reload(const struct gdtr*);
; Reloads CS via a far return, other data segs + flags by hand.
global gdt_reload
gdt_reload:
    push rbx
    mov rbx, rdi
    lgdt [rbx]
    push QWORD 0x08                     ; KCODE
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, 0x10                        ; KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    pop rbx
    ret

global tss_load
tss_load:                               ; rdi = TSS selector
    mov ax, di
    ltr ax
    ret

; --------------------------------------------------------------------------
; Interrupt stubs. struct intr_frame (ascending addresses):
;   r15..r8, rbp,rdi,rsi,rdx,rcx,rax, intno, err, rip, cs, rflags, ussp, usss
extern isr_common
extern syscall_entry

%macro PUSHALL 0
    push rbx                       ; saved first (highest address)
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POPALL 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    pop rbx
%endmacro

section .text.isr
%macro ISR_NOERR 1
isr%1:
    push QWORD 0                        ; synthetic err slot
    push QWORD %1                       ; vector
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16
    iretq
%endmacro

%macro ISR_ERR 1
isr%1:
    push QWORD %1                       ; CPU pushed err already
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16                         ; discard intno + err
    iretq
%endmacro

%assign v 0
%rep 8                                  ; 0..7 no error code
ISR_NOERR v
%assign v v+1
%endrep
ISR_ERR 8
ISR_NOERR 9
%assign v 10
%rep 5                                  ; 10..14 error code
ISR_ERR v
%assign v v+1
%endrep
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
%assign v 18
%rep 14                                 ; 18..31 no error code
ISR_NOERR v
%assign v v+1
%endrep
%assign v 32
%rep 16                                 ; IRQ0..IRQ15 -> vectors 32..47
ISR_NOERR v
%assign v v+1
%endrep

isr128:                                 ; syscall gate (ring 3)
    push QWORD 0
    push QWORD 128
    PUSHALL
    mov rdi, rsp
    cld
    call syscall_entry                  ; may block/schedule
    POPALL
    add rsp, 16
    iretq

; IPI vectors 0xF0..0xF2 — must push the real vector number, not 19.
; (idt_init used to alias 49..255 onto isr19, so a reschedule IPI was
; delivered as #XM and panicked the idle CPU.)
global isr240
isr240:                                 ; 0xF0 reschedule
    push QWORD 0
    push QWORD 240
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16
    iretq
global isr241
isr241:                                 ; 0xF1 spare
    push QWORD 0
    push QWORD 241
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16
    iretq
global isr242
isr242:                                 ; 0xF2 stop
    push QWORD 0
    push QWORD 242
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16
    iretq

global isr_stub_table
isr_stub_table:                         ; vectors 0..48 (48 == syscall gate)
%assign v 0
%rep 48
    dq isr%+v
%assign v v+1
%endrep
    dq isr128

; --------------------------------------------------------------------------
section .text

; void swtch(struct cpu_ctx **old, struct cpu_ctx *new);
; struct cpu_ctx = { r15,r14,r13,r12,rbp,rbx,rip }
global swtch
swtch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, [rsi]          ; new stack comes from *(next->ctx)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; First-entry glue for a brand-new task: its creator pushed the address of
; its interrupt frame onto its fresh kernel stack right below cpu_ctx.
global glue_first
glue_first:
    ; swtch()'s ret left rsp pointing at this task's interrupt frame
    POPALL
    add rsp, 16
    iretq

; void load_cr3(uint64_t)
global load_cr3
load_cr3:
    mov cr3, rdi
    ret
