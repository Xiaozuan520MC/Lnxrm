// SPDX-License-Identifier: Apache-2.0

/* ELF loader robustness.  elf_load() parses a buffer that came off disk,
 * so every field in it is untrusted input; REVIEW §3.2-G2 credits it with
 * seven checks and this case is what keeps them from being "simplified"
 * away.  Nothing here may map a page, allocate a frame or panic when the
 * image is bad -- all three are asserted, not assumed. */
#include <sys/ktest.h>
#include <console.h>
#include <elf.h>
#include <mm/mm.h>
#include <sys/sched.h>

enum { IMG_SIZE = 1024 };

/* A minimal but genuinely loadable ET_EXEC: one RWX PT_LOAD at
 * USER_BASE+0x1000.  p_filesz is 0 so the loader copies no bytes -- the
 * test image is not in the address space it is loaded into, and a zero
 * length copy is the only one that cannot fault.  p_offset is 0 so the
 * segment covers the ELF header and the program header table the way a
 * real image's first PT_LOAD does, which is what lets elf_get_phdrs()
 * report an AT_PHDR for it. */
static size_t build_good(u8 *img)
{
    memset(img, 0, IMG_SIZE);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    memcpy(eh->e_ident, "\x7f"
                        "ELF",
           4);
    eh->e_ident[4] = 2; /* ELFCLASS64 */
    eh->e_ident[5] = 1; /* ELFDATA2LSB */
    eh->e_ident[6] = 1; /* EV_CURRENT */
    eh->e_type = 2;     /* ET_EXEC */
    eh->e_machine = 62; /* EM_X86_64 */
    eh->e_version = 1;
    eh->e_entry = USER_BASE + 0x1000;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 1;

    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_W | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr = USER_BASE + 0x1000;
    ph->p_filesz = 0;
    ph->p_memsz = 0x1000;
    ph->p_align = 0x1000;
    return IMG_SIZE;
}

/* Positive control: without it every "rejected" below would pass just as
 * well if elf_load() returned 0 for every input.  A throwaway address space
 * keeps the frames reclaimable, so the case can assert zero net allocation. */
static void test_elf_load_accepts_good_image(void)
{
    u8 img[IMG_SIZE];
    size_t n = build_good(img);
    u64 snap = pmm_free_bytes();
    u64 brk = 0;

    u64 root = vmm_new_user_aspace();
    K_ASSERT(root != 0);

    u64 entry = elf_load(root, img, n, &brk);
    K_EXPECT_EQ(entry, USER_BASE + 0x1000);
    K_EXPECT_EQ(brk, ALIGN_UP(USER_BASE + 0x1000 + 0x1000, PAGE_SIZE));

    /* the segment's page is present, page-aligned and offset-preserving */
    u64 pa = vmm_translate_in(root, USER_BASE + 0x1000);
    K_EXPECT(pa != 0);
    K_EXPECT_EQ(pa & (PAGE_SIZE - 1), 0);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE + 0x1000 + 0x123),
                pa + 0x123);
    /* nothing lives past the segment */
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE + 0x2000), 0);

    vmm_destroy_user_aspace(root);
    K_EXPECT_EQ(pmm_free_bytes(), snap); /* rejected or not: no frame leaks */
}

/* The rejection table.  Each case rebuilds the good image, breaks exactly
 * one thing, and requires: return 0, no frame touched, *brk_end untouched. */
static void expect_reject(const char *what, void (*break_it)(u8 *img))
{
    u8 img[IMG_SIZE];
    size_t n = build_good(img);
    u64 brk = 0xdeadbeef;

    break_it(img);
    u64 entry = elf_load(current->pml4, img, n, &brk);
    if (entry || brk != 0xdeadbeef)
        kprintf("[ktest] elf did not reject %s cleanly (entry=0x%lx)\n", what, entry);
    K_EXPECT_EQ(entry, 0);
    K_EXPECT_EQ(brk, 0xdeadbeef); /* rejected before any side effect */
}

static void brk_magic(u8 *img) { img[0] = 'X'; }
static void brk_class32(u8 *img) { ((Elf64_Ehdr *)img)->e_ident[4] = 1; }
static void brk_dyn(u8 *img) { ((Elf64_Ehdr *)img)->e_type = 3; }
static void brk_entry_low(u8 *img) { ((Elf64_Ehdr *)img)->e_entry = 0; }
static void brk_entry_kernel(u8 *img)
{
    ((Elf64_Ehdr *)img)->e_entry = PHYS_TO_VIRT(0x100000);
}
static void brk_entry_past(u8 *img)
{
    ((Elf64_Ehdr *)img)->e_entry = USER_MAX_VMA + 0x1000;
}
static void brk_phoff(u8 *img) { ((Elf64_Ehdr *)img)->e_phoff = IMG_SIZE + 1; }
static void brk_phnum_cap(u8 *img) { ((Elf64_Ehdr *)img)->e_phnum = 200; }
static void brk_phnum_wrap(u8 *img) { ((Elf64_Ehdr *)img)->e_phnum = 0xffff; }
static void brk_filesz_gt_memsz(u8 *img)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph->p_memsz = 0x100;
    ph->p_filesz = 0x200;
}
static void brk_offset_past(u8 *img)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph->p_offset = IMG_SIZE - 4;
    ph->p_filesz = 0x40; /* p_offset + p_filesz runs past the image */
}

static void test_elf_load_rejects(void)
{
    u64 snap = pmm_free_bytes();
    u8 img[IMG_SIZE];
    u64 brk = 0xdeadbeef;

    /* header itself shorter than an Ehdr */
    K_EXPECT_EQ(elf_load(current->pml4, img, 32, &brk), 0);
    K_EXPECT_EQ(brk, 0xdeadbeef);

    /* no image at all: a NULL buffer with a plausible size must be rejected
     * on the way in, not dereferenced at address 0 */
    K_EXPECT_EQ(elf_load(current->pml4, NULL, IMG_SIZE, &brk), 0);
    K_EXPECT_EQ(brk, 0xdeadbeef);

    expect_reject("bad magic", brk_magic);
    expect_reject("ELFCLASS32", brk_class32);
    expect_reject("e_type != ET_EXEC", brk_dyn);
    expect_reject("e_entry = 0", brk_entry_low);
    expect_reject("e_entry in kernel space", brk_entry_kernel);
    expect_reject("e_entry past USER_MAX_VMA", brk_entry_past);
    expect_reject("e_phoff outside the image", brk_phoff);
    expect_reject("e_phnum > 128", brk_phnum_cap);
    expect_reject("e_phnum * phentsize overflows", brk_phnum_wrap);
    expect_reject("p_filesz > p_memsz", brk_filesz_gt_memsz);
    expect_reject("p_offset + p_filesz past image", brk_offset_past);

    K_EXPECT_EQ(pmm_free_bytes(), snap); /* no rejection allocated anything */
}

KTEST("elf", KTEST_LATE, test_elf_load_accepts_good_image);
KTEST("elf", KTEST_LATE, test_elf_load_rejects);

/* ================= S3: execve's three helpers =================
 * elf_interp_path() decides WHICH images execve loads, elf_get_phdrs()
 * fills the auxiliary vector the dynamic linker cannot start without, and
 * elf_image_range() is what keeps the two images from being loaded on top
 * of each other.  All three parse the same untrusted buffer as elf_load()
 * does, so each is probed with a well-formed image, a deliberately broken
 * one, and the boundary they share with the loader. */

/* A PT_INTERP segment at a caller-chosen offset, carrying `len` bytes. */
static void add_interp(u8 *img, u64 off, u64 len)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    eh->e_phnum = 2;
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph[1].p_type = PT_INTERP;
    ph[1].p_offset = off;
    ph[1].p_filesz = len;
}

static void test_elf_interp_path(void)
{
    u8 img[IMG_SIZE];
    size_t n = build_good(img);
    char path[64];

    /* no PT_INTERP: a static image, and 0 rather than an error -- execve
     * must not refuse a program just because it needs no interpreter */
    K_EXPECT_EQ(elf_interp_path(img, n, path, sizeof path), 0);

    add_interp(img, 0x300, 10);
    memcpy(img + 0x300, "/lib/ld.so", 10);
    K_EXPECT_EQ(elf_interp_path(img, n, path, sizeof path), 1);
    K_EXPECT(strcmp(path, "/lib/ld.so") == 0);

    /* the path runs off the end of the file: corrupt, not truncated to
     * four bytes of a name that would then name nothing */
    add_interp(img, IMG_SIZE - 4, 10);
    K_EXPECT_EQ(elf_interp_path(img, n, path, sizeof path), -1);

    /* fits the file but not the caller's buffer */
    add_interp(img, 0x300, 10);
    K_EXPECT_EQ(elf_interp_path(img, n, path, 4), -1);

    /* nowhere to put the result */
    K_EXPECT_EQ(elf_interp_path(img, n, NULL, sizeof path), -1);

    /* header table itself unreadable */
    K_EXPECT_EQ(elf_interp_path(img, 32, path, sizeof path), -1);
}

static void test_elf_get_phdrs(void)
{
    u8 img[IMG_SIZE];
    size_t n = build_good(img);
    struct elf_phdrs ph;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    Elf64_Phdr *p = (Elf64_Phdr *)(img + eh->e_phoff);

    /* build_good's segment carries no file bytes, so it holds no header
     * table either: the honest AT_PHDR is 0 and that is not an error */
    K_EXPECT_EQ(elf_get_phdrs(img, n, &ph), 0);
    K_EXPECT_EQ(ph.vaddr, 0);
    K_EXPECT_EQ(ph.entsize, sizeof(Elf64_Phdr));
    K_EXPECT_EQ(ph.count, 1);

    /* once the file range covers the table, AT_PHDR is where the loader
     * put it: p_vaddr + (e_phoff - p_offset) */
    p->p_filesz = 0x200;
    K_EXPECT_EQ(elf_get_phdrs(img, n, &ph), 0);
    K_EXPECT_EQ(ph.vaddr, USER_BASE + 0x1000 + sizeof(Elf64_Ehdr));

    /* a table below USER_BASE is never mapped (elf_load skips the whole
     * segment), so there is nothing to point AT_PHDR at */
    p->p_vaddr = 0x200000;
    K_EXPECT_EQ(elf_get_phdrs(img, n, &ph), 0);
    K_EXPECT_EQ(ph.vaddr, 0);

    p->p_vaddr = USER_BASE + 0x1000; /* restore for the malformed case */
    K_EXPECT_EQ(elf_get_phdrs(img, 32, &ph), -1);
    K_EXPECT_EQ(elf_get_phdrs(img, n, NULL), -1);
}

static void test_elf_image_range(void)
{
    u8 a[IMG_SIZE], b[IMG_SIZE];
    u64 lo = 0, hi = 0, lo2 = 0, hi2 = 0;
    size_t n = build_good(a);

    /* the span is exactly what elf_load maps for this image -- the two
     * must agree, or a range check could certify a collision as safe */
    K_EXPECT_EQ(elf_image_range(a, n, &lo, &hi), 0);
    K_EXPECT_EQ(lo, USER_BASE + 0x1000);
    K_EXPECT_EQ(hi, ALIGN_UP(USER_BASE + 0x1000 + 0x1000, PAGE_SIZE));

    build_good(b);
    Elf64_Phdr *pb = (Elf64_Phdr *)(b + ((Elf64_Ehdr *)b)->e_phoff);

    /* a second image whose span overlaps: elf_load would REUSE the pages
     * already mapped rather than fail, merging both images' bytes, so the
     * overlap has to be caught here -- by the same predicate execve uses */
    pb->p_vaddr = USER_BASE + 0x1800;
    K_EXPECT_EQ(elf_image_range(b, IMG_SIZE, &lo2, &hi2), 0);
    K_EXPECT(lo < hi2 && lo2 < hi);

    /* one that only abuts is fine: [0x1000,0x2000) and [0x2000,0x3000) */
    pb->p_vaddr = USER_BASE + 0x2000;
    K_EXPECT_EQ(elf_image_range(b, IMG_SIZE, &lo2, &hi2), 0);
    K_EXPECT(!(lo < hi2 && lo2 < hi));

    /* a segment below USER_BASE is never mapped, so it occupies nothing */
    pb->p_vaddr = 0x200000;
    K_EXPECT_EQ(elf_image_range(b, IMG_SIZE, &lo2, &hi2), -1);

    /* nothing loadable at all, and unreadable input, are both -1 */
    build_good(b);
    pb = (Elf64_Phdr *)(b + ((Elf64_Ehdr *)b)->e_phoff);
    pb->p_type = PT_DYNAMIC; /* any non-PT_LOAD segment is invisible here */
    K_EXPECT_EQ(elf_image_range(b, IMG_SIZE, &lo2, &hi2), -1);
    K_EXPECT_EQ(elf_image_range(b, 32, &lo2, &hi2), -1);
    K_EXPECT_EQ(elf_image_range(b, IMG_SIZE, NULL, &hi2), -1);
}

KTEST("elf", KTEST_LATE, test_elf_interp_path);
KTEST("elf", KTEST_LATE, test_elf_get_phdrs);
KTEST("elf", KTEST_LATE, test_elf_image_range);
