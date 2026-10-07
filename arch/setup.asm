; lnxrm -- bzImage real-mode setup
; Layout follows the Linux x86 boot protocol (Documentation/arch/x86/boot.rst):
;   offset 0x1F1 setup_sects, 0x202 "HdrS", 0x200 entry point.
; Loaded by SeaBIOS/QEMU at 0x9000:0000, entered at 0x9000:0200.
; Job: enable A20 -> protected mode -> build temporary page tables ->
;      activate long mode -> far jump to the 64-bit kernel at 0x100000.

BITS 16
ORG 0

; No boot sector is required for -kernel loading; QEMU/SeaBIOS enter at
; offset 0x200. Pad sector 0 so the header lands at the protocol offsets.
times 0x1F1-($-$$) db 0
setup_sects:    db 0                ; patched by scripts/patch_bzimage.py
                dw 0                ; 0x1F2 root_flags
syssize:        dd 0                ; 0x1F4 payload size /16, patched
                dw 0                ; 0x1F8 ram_size
                dw 0xFFFF           ; 0x1FA vid_mode = ask
                dw 0                ; 0x1FC root_dev
boot_flag:      dw 0xAA55           ; 0x1FE

entry_jump:     jmp short real_start    ; 0x200: loader enters here
                                ; 0x202 header signature
                db 'H','d','r','S'
version:        dw 0x0208           ; protocol 2.08
realmode_swtch: dd 0                ; none
start_sys_seg:  dw 0x1000
kernel_version: dw kvstr - $$       ; offset of version string
type_of_loader: db 0
loadflags:      db 0x81             ; LOADED_HIGH | CAN_USE_HEAP
setup_move_size: dw 0x8000
code32_start:   dd 0x100000         ; payload load address
ramdisk_image:  dd 0
ramdisk_size:   dd 0
bootsect_kludge: dd 0
heap_end_ptr:   dw 0xEE00
ext_loader_ver: db 0
ext_loader_type: db 0
cmd_line_ptr:   dd 0                ; filled by loader
initrd_addr_max: dd 0x7FFFFFFF
kernel_alignment: dd 16
relocatable_kernel: db 0
min_alignment:  db 0
xloadflags:     dw 1                ; XLF_KERNEL_64
cmdline_size:   dd 2048
hardware_subarch: dd 0
hw_subarch_data: dq 0
payload_offset: dd 0
payload_length: dd 0
setup_data:     dq 0
pref_address:   dq 0
init_size:      dd 0x400000
handover_offset: dd 0

times 0x270-($-$$) db 0             ; canary: overrunning the header fails here

; ------------------------------------------------------------ real mode ---
; Scratch + handover area for the VBE probe (flat addressing, DS = ES = 0).
; 0x8000: mode info buffer, 0x8400: controller info, 0x8B00: scratch,
; 0x8C00: struct vbe_lfb_info handed to the kernel (see include/boot.h).
VBE_LIST   equ 0x8B00          ; dd  cursor while walking the BIOS mode list
VBE_MODE   equ 0x8B04          ; dw  mode number being probed
VBE_MINBPP equ 0x8B06          ; db  depth floor for the current pass
VBE_IDX    equ 0x8B07          ; db  index into the classic-mode list
VBE_REQ_W  equ 0x8B08          ; dw  required X resolution (0 = any)
VBE_REQ_H  equ 0x8B0A          ; dw  required Y resolution (0 = any)
VBE_TOTMEM equ 0x8B0C          ; dw  VRAM in 64 KiB units (0 = unknown)
VBE_BEST   equ 0x8B0E          ; dd  pixel count of the best mode seen so far
VBE_BESTMD equ 0x8B12          ; dw  mode number that produced it
VBE_LFB    equ 0x8C00          ; dd  PhysBasePtr
VBE_PITCH  equ 0x8C04          ; dd  BytesPerScanLine
VBE_XRES   equ 0x8C08          ; dw  XResolution
VBE_YRES   equ 0x8C0A          ; dw  YResolution
VBE_BPP    equ 0x8C0C          ; db  BitsPerPixel
VBE_OK     equ 0x8C0D          ; db  1 when a mode is live
VBE_MM     equ 0x8C0E          ; db  MemoryModel (4 packed / 6 direct)
VBE_RPOS   equ 0x8C0F          ; db  RedFieldPosition
VBE_RSIZE  equ 0x8C10          ; db  RedMaskSize
VBE_GPOS   equ 0x8C11          ; db  GreenFieldPosition
VBE_GSIZE  equ 0x8C12          ; db  GreenMaskSize
VBE_BPOS   equ 0x8C13          ; db  BlueFieldPosition
VBE_BSIZE  equ 0x8C14          ; db  BlueMaskSize
VBE_MODENO equ 0x8C15          ; dw  VBE mode number

%macro DBG 1
    mov al, %1
    out 0xE9, al
%endmacro

real_start:
    cli
    cld
    DBG '1'
    mov ax, cs          ; debug: dump CS
    out 0xE9, al
    mov al, ah
    out 0xE9, al 
    ; Derive our own segment: we were entered at SEG:0x200 where SEG holds
    ; file offset 0, so the setup base segment is CS-0x20.
    mov ax, cs
    sub ax, 0x20
    mov ds, ax
    mov es, ax

    mov ax, ds                      ; stack inside the setup segment
    mov ss, ax
    mov sp, 0xEE00                  ; below heap_end_ptr, linear 0x9EE00

    ; ---- enable A20 ----
    DBG '2'
    mov ax, 0x2401                  ; BIOS: enable A20
    int 0x15                        ; clobbers registers -- already stashed
    in  al, 0x92                    ; fast A20 gate as fallback
    or  al, 2
    and al, 0xFE
    out 0x92, al
    DBG '3'

    ; ---- capture the E820 memory map ourselves -------------------------
    ; table stored at linear 0x7000, dword count at 0x6FFC
    cli
    xor eax, eax
    mov es, ax                      ; ES = 0 for the buffer
    mov di, 0x7000
    xor ebx, ebx                    ; continuation
    mov DWORD [es:0x6FFC], 0        ; counter
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, 20
    int 0x15
    jc  .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    add di, 20
    inc DWORD [es:0x6FFC]
    test ebx, ebx
    jz  .e820_done
    cmp DWORD [es:0x6FFC], 64
    jb  .e820_loop
.e820_done:
    ; NOTE: interrupts stay OFF all the way until start_kernel's final sti

    ; ---- VBE: pick a linear-framebuffer graphics mode --------------------
    ; Real mode with DS = ES = 0 => flat physical addressing (VBE_* equates).
    ;   pass 0  preferred resolutions, deepest colour first:
    ;           1280x720  (720p)  at 32, 24, then any true colour
    ;           1920x1080 (1080p) at 32, 24, then any true colour
    ;   pass 1  largest true-colour mode that fits the VRAM (adaptive)
    ;   pass A  classic VESA 8bpp LFB modes 0x101 / 0x103 / 0x105
    ;   pass B  first BIOS mode with any supported depth
    ; A candidate must survive 4F01 validation AND a real 4F02 mode set, so
    ; a BIOS that advertises a mode it cannot program is not trusted.
    ; Every mode must also fit VRAM (4F00 TotalMemory) and the 16 MiB LFB
    ; window the kernel maps, so the choice never outgrows the framebuffer.
    ; The mode that wins is described at 0x8C00 (struct vbe_lfb_info).
    DBG 'V'
    push ds
    xor ax, ax
    mov ds, ax                      ; DS = 0 for flat real-mode addressing
    mov es, ax                      ; ES = 0 (INT 0x10 VBE uses ES:DI)
    mov byte [VBE_OK], 0
    mov byte [VBE_IDX], 0
    mov byte [VBE_MINBPP], 0
    mov word [VBE_REQ_W], 0
    mov word [VBE_REQ_H], 0
    mov word [VBE_TOTMEM], 0

    ; controller info once: how much VRAM the BIOS reports (64 KiB units),
    ; used by vbe_probe to reject modes whose scan buffer would not fit.
    mov ax, 0x4F00                  ; VBE: Get Controller Information
    mov di, 0x8400                  ; buffer at physical 0x8400
    int 0x10
    cmp ax, 0x004F
    jne .no_ctl
    cmp byte [0x8400], 'V'          ; 'VESA'
    jne .no_ctl
    cmp byte [0x8401], 'E'
    jne .no_ctl
    mov ax, [0x8412]                ; TotalMemory
    mov [VBE_TOTMEM], ax
.no_ctl:

    ; ---- preferred resolutions, deepest colour first ---------------------
    ; VBE_TRY_MODE w, h: accept this exact mode at 32, then 24, then any
    ; true colour depth the machine can offer.  720p is the default and
    ; wins whenever the BIOS offers it; 1080p only takes over on machines
    ; that have no 720p mode at all, and neither ever falls back silently
    ; to a bigger mode.
%macro VBE_TRY_MODE 2
    mov word [VBE_REQ_W], %1
    mov word [VBE_REQ_H], %2
    mov byte [VBE_MINBPP], 32       ; 32 bpp (xRGB8888) first ...
    call vbe_enum
    jnc vbe_end
    mov byte [VBE_MINBPP], 24       ; ... 24 bpp (RGB888) ...
    call vbe_enum
    jnc vbe_end
    mov byte [VBE_MINBPP], 15       ; ... then any true-colour depth
    call vbe_enum
    jnc vbe_end
%endmacro

    VBE_TRY_MODE 1280, 720          ; 720p  (default)
    VBE_TRY_MODE 1920, 1080         ; 1080p (fallback)
    mov word [VBE_REQ_W], 0         ; no exact size anymore: adaptive pass
    mov word [VBE_REQ_H], 0

    ; ---- adaptive fallback: the biggest true-colour mode we can use ------
    mov byte [VBE_MINBPP], 15
    call vbe_enum_best
    jnc vbe_end

    mov byte [VBE_MINBPP], 0

vbe_pass_a:
    movzx eax, byte [VBE_IDX]
    cmp al, 3
    jae vbe_pass_b
    inc byte [VBE_IDX]
    shl ax, 1                       ; 0x101 (640x480), 0x103, 0x105
    add ax, 0x0101
    mov [VBE_MODE], ax
    call vbe_probe
    jc vbe_pass_a
    call vbe_setmode
    jc vbe_pass_a
    call vbe_save
    jmp vbe_end

vbe_pass_b:
    mov byte [VBE_MINBPP], 0        ; anything we can draw with
    call vbe_enum
    jnc vbe_end
    mov byte [VBE_OK], 0            ; no usable mode: stay in VGA text mode
    jmp vbe_end

    align 2
    ; ---- vbe_probe: 4F01 [VBE_MODE] -> info at 0x8000, CF=0 when usable ---
    ; Also enforces the current selection criteria: VBE_MINBPP is the depth
    ; floor, VBE_REQ_W/VBE_REQ_H an exact resolution (0 = accept any size),
    ; and the mode's scan buffer must fit in the VRAM reported by 4F00.
vbe_probe:
    mov cx, [VBE_MODE]
    mov ax, 0x4F01                  ; VBE: Get Mode Information
    mov di, 0x8000                  ; buffer at physical 0x8000
    int 0x10
    cmp ax, 0x004F
    jne .bad
    mov ax, [0x8000]                ; ModeAttributes
    test al, 0x02                   ; supported by hardware
    jz .bad
    test al, 0x80                   ; linear framebuffer available
    jz .bad
    test al, 0x10                   ; graphics mode, not text
    jz .bad
    movzx eax, word [0x8012]        ; XResolution
    mov dx, [VBE_REQ_W]
    test dx, dx
    jz .w_any
    cmp ax, dx
    jne .bad
.w_any:
    cmp eax, 320
    jb .bad
    cmp eax, 4096
    ja .bad
    movzx eax, word [0x8014]        ; YResolution
    mov dx, [VBE_REQ_H]
    test dx, dx
    jz .h_any
    cmp ax, dx
    jne .bad
.h_any:
    cmp eax, 200
    jb .bad
    cmp eax, 4096
    ja .bad
    cmp dword [0x8028], 0           ; PhysBasePtr
    je .bad
    mov al, [0x8019]                ; BitsPerPixel
    mov ah, [VBE_MINBPP]
    cmp al, ah
    jb .bad
    cmp al, 8
    je .depth_ok
    cmp al, 15
    je .depth_ok
    cmp al, 16
    je .depth_ok
    cmp al, 24
    je .depth_ok
    cmp al, 32
    jne .bad
.depth_ok:
    mov al, [0x801B]                ; MemoryModel
    cmp al, 4                       ; packed pixel
    je .mem_ok
    cmp al, 6                       ; direct colour
    jne .bad
.mem_ok:
    ; the scan buffer must fit in the VRAM behind it (64 KiB units, VBE 2.0)
    movzx eax, word [0x8010]        ; BytesPerScanLine
    movzx edx, word [0x8014]        ; YResolution
    mul edx                         ; EDX:EAX = pitch * height
    test edx, edx
    jnz .bad                        ; > 4 GiB: bogus mode info
    mov ecx, eax                    ; ECX = frame bytes
    cmp ecx, 0x1000000              ; <= 16 MiB: the kernel's LFB window
    ja .bad                         ; (FB_MAX_BYTES in include/framebuffer.h)
    movzx eax, word [VBE_TOTMEM]    ; VRAM in 64 KiB units (0 = unknown)
    test eax, eax
    jz .good
    shl eax, 16                     ; -> bytes
    jc .good                        ; >= 4 GiB of VRAM: certainly fits
    cmp ecx, eax
    ja .bad
.good:
    clc
    ret
.bad:
    stc
    ret

    ; ---- vbe_setmode: 4F02 [VBE_MODE] with the LFB flag, CF=0 on success --
vbe_setmode:
    mov bx, [VBE_MODE]
    or  bx, 0x4000                  ; bit 14 = use the linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .ok
    stc
    ret
.ok:
    clc
    ret

    ; ---- vbe_save: describe the mode we just set at 0x8C00 ---------------
vbe_save:
    mov eax, [0x8028]               ; PhysBasePtr
    mov [VBE_LFB], eax
    movzx eax, word [0x8010]        ; BytesPerScanLine
    mov [VBE_PITCH], eax
    movzx eax, word [0x8012]        ; XResolution
    mov [VBE_XRES], ax
    movzx eax, word [0x8014]        ; YResolution
    mov [VBE_YRES], ax
    mov al, [0x8019]                ; BitsPerPixel
    mov [VBE_BPP], al
    mov al, [0x801B]                ; MemoryModel
    mov [VBE_MM], al
    mov al, [0x8020]                ; RedFieldPosition
    mov [VBE_RPOS], al
    mov al, [0x801F]                ; RedMaskSize
    mov [VBE_RSIZE], al
    mov al, [0x8022]                ; GreenFieldPosition
    mov [VBE_GPOS], al
    mov al, [0x8021]                ; GreenMaskSize
    mov [VBE_GSIZE], al
    mov al, [0x8024]                ; BlueFieldPosition
    mov [VBE_BPOS], al
    mov al, [0x8023]                ; BlueMaskSize
    mov [VBE_BSIZE], al
    mov ax, [VBE_MODE]
    mov [VBE_MODENO], ax
    mov byte [VBE_OK], 1
    ret

    ; ---- vbe_enum: walk the BIOS mode list, take the first mode that works
vbe_enum:
    mov ax, 0x4F00                  ; VBE: Get Controller Information
    mov di, 0x8400                  ; buffer at physical 0x8400 (512 bytes)
    int 0x10
    cmp ax, 0x004F
    jne .none
    cmp byte [0x8400], 'V'          ; 'VESA' signature
    jne .none
    cmp byte [0x8401], 'E'
    jne .none
    ; VideoModePtr is a far pointer at offset 0x0E: offset word, then segment
    movzx esi, word [0x8410]        ; segment
    shl esi, 4                      ; * 16
    movzx eax, word [0x840E]        ; offset
    add esi, eax                    ; ESI = linear address of the mode list
    mov [VBE_LIST], esi             ; INT 0x10 may clobber registers
.next:
    mov esi, [VBE_LIST]
    movzx ecx, word [esi]           ; load mode number
    add esi, 2
    mov [VBE_LIST], esi
    cmp cx, 0xFFFF                  ; end-of-list?
    je .none
    mov [VBE_MODE], cx
    call vbe_probe
    jc .next
    call vbe_setmode
    jc .next
    call vbe_save
    clc
    ret
.none:
    stc
    ret

    ; ---- vbe_enum_best: the LARGEST mode matching the current criteria ----
    ; vbe_enum takes the first mode the BIOS lists (usually the smallest one).
    ; When neither 1080p nor 720p is offered, picking the biggest true-colour
    ; mode keeps the console as usable as the hardware allows.
vbe_enum_best:
    mov dword [VBE_BEST], 0         ; pixel count of the best mode so far
    mov word [VBE_BESTMD], 0
    mov ax, 0x4F00                  ; VBE: Get Controller Information
    mov di, 0x8400                  ; buffer at physical 0x8400 (512 bytes)
    int 0x10
    cmp ax, 0x004F
    jne .none
    cmp byte [0x8400], 'V'          ; 'VESA' signature
    jne .none
    cmp byte [0x8401], 'E'
    jne .none
    movzx esi, word [0x8410]        ; mode list segment:offset at 0x10 / 0x0E
    shl esi, 4
    movzx eax, word [0x840E]
    add esi, eax
    mov [VBE_LIST], esi
.next:
    mov esi, [VBE_LIST]
    movzx ecx, word [esi]
    add esi, 2
    mov [VBE_LIST], esi
    cmp cx, 0xFFFF                  ; end-of-list?
    je .apply
    mov [VBE_MODE], cx
    call vbe_probe                  ; applies size / depth / VRAM criteria
    jc .next
    movzx eax, word [0x8012]        ; XResolution
    movzx edx, word [0x8014]        ; YResolution
    mul edx                         ; EDX:EAX = pixels (both <= 4096)
    cmp eax, [VBE_BEST]
    jbe .next                       ; not bigger than what we already have
    mov [VBE_BEST], eax
    mov ax, [VBE_MODE]
    mov [VBE_BESTMD], ax
    jmp .next
.apply:
    cmp word [VBE_BESTMD], 0
    je .none
    mov ax, [VBE_BESTMD]
    mov [VBE_MODE], ax
    call vbe_probe                  ; reload that mode's info block at 0x8000
    jc .none
    call vbe_setmode
    jc .none
    call vbe_save
    clc
    ret
.none:
    stc
    ret

vbe_end:
    pop ds

    ; ---- protected mode ----
    cli
    DBG '4'
    ; patch the GDT descriptor's base with the linear address of |gdt|
    xor eax, eax
    mov ax, ds
    shl eax, 4
    add [gdtdesc+2], eax
    lgdt [gdtdesc]
    DBG '5'
    mov eax, cr0
    or  eax, 1                      ; CR0.PE
    mov cr0, eax
    ; Patch the protected-mode far jump: NASM emits a link-time offset,
    ; but we need the runtime-linear address (setup base + offset).
    xor eax, eax
    mov ax, ds
    shl eax, 4
    add eax, pm_entry
    mov [pm_jmp+2], eax
    DBG '6'
pm_jmp:
    jmp dword 0x08:0xF00DFACE       ; patched above

BITS 32
pm_entry:
    DBG '7'
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x7C00

    ; ---- temporary page tables @0x60000 -----------------------------------
    ; pml4[0]  -> pdpt (identity 1 GiB)
    ; pml4[511]-> same pdpt, whose entry 510 maps the high half (2 MiB pages)
    ; pdpt@0x61000, pd_low@0x62000, pd_high@0x63000
    mov DWORD [0x60000+0*8],   0x61003
    mov DWORD [0x60000+511*8], 0x61003
    mov DWORD [0x61000+0*8],   0x62003
    mov DWORD [0x61000+510*8], 0x63003

    ; identity map low 1 GiB with 2 MiB pages
    mov edi, 0x62000
    mov eax, 0x83                   ; P|W|PS, phys 0
    mov ecx, 512
.idloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .idloop

    ; high half: kernel image first 32 x 2 MiB => 64 MiB window
    mov edi, 0x63000
    mov eax, 0x83                   ; phys 0 (kernel is at 1 MiB)
    mov ecx, 32
.hiloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .hiloop
    ; zero remaining pd_high entries
    mov ecx, 480
.zloop:
    mov DWORD [edi], 0
    mov DWORD [edi+4], 0
    add edi, 8
    loop .zloop

    ; VGA text window (PD index 192) -> PT @0x64000 -> phys 0xB8000
    mov DWORD [0x63000+96*8], 0x64003
    mov DWORD [0x64000], 0xB8003

    ; ---- long mode ----
    mov eax, cr4
    or  eax, 0x20                   ; CR4.PAE
    mov cr4, eax
    mov edx, 0x60000
    mov cr3, edx
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or  eax, 0x900                  ; EFER.LME (8) | EFER.NXE (11)
    wrmsr
    mov eax, cr0
    or  eax, 0x80010000             ; CR0.PG | CR0.WP
    mov cr0, eax

    mov DWORD [0x6F00], 0

pm_jmp64:
    jmp 0x18:0xF00DFACE             ; patched by build -> _start64 physical

; ------------------------------------------------------------------ data ---
align 8
gdt:
    dq 0                            ; null
    dq 0x00CF9A000000FFFF           ; 0x08 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10 data
    dq 0x00209A0000000000           ; 0x18 64-bit code (L=1)
gdtdesc:
    dw gdtdesc - gdt - 1
    dd gdt

kvstr:      db "lnxrm v1.0 (x86_64)",0
