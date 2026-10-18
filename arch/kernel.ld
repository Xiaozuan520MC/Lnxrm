/* eck-system-kernel link script.
 * One contiguous PROGBITS image so VMA/LMA stay perfectly in step:
 * every byte satisfies VMA = LMA + (KERNEL_VMA - KERNEL_LMA).
 * objcopy -O binary dumps by LMA => payload starts at physical 0x100000. */

ENTRY(_start64)

KERNEL_LMA = 0x100000;
KERNEL_VMA = 0xffffffff80100000;

SECTIONS
{
    . = KERNEL_VMA;

    .image : AT(KERNEL_LMA)
    {
        __kernel_start = .;
        /* --gc-sections must never drop the boot entry: ENTRY(_start64)
         * roots this section, but KEEP makes that explicit -- _start32
         * lives here too and scripts/patch_setup_jmp.py needs BOTH symbols
         * to compute the setup.bin long-mode jump target. */
        KEEP(*entry64.o(.text.prologue))
        *(.text .text.*)

        *(.rodata .rodata.*)

        __init_array_start = .;
        KEEP(*(.init_array*))
        KEEP(*(.fini_array*))
        __init_array_end = .;

        *(.data .data.*)

        /* Boot self-test cases (include/sys/ktest.h).  KEEP is mandatory:
         * nothing outside this section references the cases, so
         * --gc-sections would otherwise delete every test in the image and
         * the kernel would "pass" a suite it no longer contains. */
        . = ALIGN(8);
        __ktest_start = .;
        KEEP(*(.ktest))
        __ktest_end = .;

        /* The initramfs (`make INITRAMFS=`): an objcopy'd newc cpio archive,
         * so it is real payload that travels inside bzImage.  KEEP is as
         * mandatory as it is for .ktest -- nothing in the kernel references
         * these bytes by name (fs/initramfs.c reaches them through the two
         * symbols below), so --gc-sections would delete the archive the user
         * asked for and leave those symbols pointing at whatever followed.
         * With no INITRAMFS= nothing contributes the section and the two
         * symbols coincide, which is how fs/initramfs.c tells "no archive"
         * from "an empty one".  It sits inside .image, hence inside the
         * [1 MiB, __bss_end) range pmm_init() keeps out of the buddy. */
        . = ALIGN(4);
        __initramfs_start = .;
        KEEP(*(.initramfs))
        __initramfs_end = .;

        . = ALIGN(16);
    }

    .bss : AT(LOADADDR(.image) + SIZEOF(.image))
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        . += 0x4000;                 /* boot stack */
        __boot_stack_top = .;
        __bss_end = .;
    }

    __kernel_end_lma = LOADADDR(.bss) + SIZEOF(.bss);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
        *(.comment)
    }
}
