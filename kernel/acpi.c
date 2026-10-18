/* Minimal ACPI: locate the RSDP, walk RSDT/XSDT to the MADT (APIC table),
 * and expose the two facts the rest of the kernel needs from it: which
 * local APIC IDs exist, and where the IOAPIC sits.
 *
 * Why this exists: enumerating APs by probing APIC IDs 1..N works on QEMU
 * (IDs are 0,1,2..) but real machines hand out topology-encoded IDs with
 * holes (0,4,...) — the probe's "2 consecutive failures -> stop" rule then
 * gives up before it ever tries the second core's real ID, and the machine
 * boots single-core.  The MADT is the architecturally correct list; every
 * general-purpose kernel reads it for exactly this reason.
 *
 * The RSDP lives in the BIOS areas below 1 MiB and is read directly
 * through the identity map.  The tables it points at are NOT low-memory
 * citizens: real firmware parks them at the top of low RAM — routinely
 * above the identity window, where a plain dereference either faults or
 * (past 2 GiB, through the linear alias) reads an unrelated frame.  That
 * mismatch is exactly what black-screened real machines >2 GiB while QEMU
 * (SeaBIOS keeps every table below 1 MiB) stayed green, so every table
 * read goes through the one-page bounce window at ACPI_SCRATCH_VA
 * (vmm_map_kernel_page exists: acpi_init() runs after vmm_init()).
 * Failure (no RSDP, no MADT, broken checksums, a table too big to copy —
 * old or exotic firmware) is never fatal; it only means the caller keeps
 * its probe fallback. */
#include <sys/acpi.h>
#include <console.h>
#include <mm/mm.h> /* vmm_map_kernel_page, PG_P/PG_W */

/* ---- ACPI tables (ACPI spec; x86 is little-endian) ---- */

struct rsdp {
    char sig[8];       /* "RSD PTR " */
    u8   checksum;     /* of the first 20 bytes */
    char oem[6];
    u8   revision;     /* 0: RSDT only; >= 2: XSDT also valid */
    u32  rsdt;         /* physical address of the RSDT — offset 16, the
                        * rev-0 struct is exactly 20 bytes and has NO
                        * padding between revision and this field */
    /* rev >= 2: */
    u32  length;       /* total struct length (offset 20) */
    u64  xsdt;         /* physical address of the XSDT (offset 24) */
    u8   ext_checksum; /* of `length` bytes (offset 32) */
    u8   _pad[3];
} __attribute__((packed));

struct sdt_hdr {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem[6];
    char oem_table[8];
    u32  oem_revision;
    u32  creator;
    u32  creator_revision;
} __attribute__((packed)); /* 36 bytes, MADT body starts right after */

#define MADT_BODY_MIN 44 /* header (36) + lapic addr (4) + flags (4) */

static u32 s_ap_ids[MAX_CPUS]; /* enabled local APIC IDs, deduplicated */
static u32 s_ap_count;         /* 0 = no usable MADT found */
static u32 s_ioapic_base;

static bool sum_ok(const void *p, u32 len)
{
    const u8 *b = p;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + b[i]);
    return sum == 0;
}

static bool sig_is(const char *sig, const char *want)
{
    for (int i = 0; i < 4; i++)
        if (sig[i] != want[i]) return false;
    return true;
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---- RSDP ---- */

/* BDA word 0x40E: the EBDA base as a segment.  Read with a literal-address
 * mov because any C-level integer->pointer dereference of low memory makes
 * GCC -Warray-bounds claim a zero-length object lives there — it re-raises
 * the warning even through a noinline parameter (IPA clones the function
 * specialized to the constant).  The assembler text is opaque to that
 * analysis.  This runs once, early, through the low-memory identity map. */
static u16 bda_word(void)
{
    u32 v; /* 32-bit dest: movzwl requires a full register */
    __asm__ volatile("movzwl 0x40e, %0" : "=r"(v));
    return (u16)v;
}

static const struct rsdp *scan_rsdp(u64 start, u64 end)
{
    for (u64 a = start; a < end; a += 16) {
        const struct rsdp *r = (const struct rsdp *)a;
        if (r->sig[0] != 'R' || r->sig[1] != 'S' || r->sig[2] != 'D' ||
            r->sig[3] != ' ' || r->sig[4] != 'P' || r->sig[5] != 'T' ||
            r->sig[6] != 'R' || r->sig[7] != ' ')
            continue;
        if (!sum_ok(r, 20)) continue;
        return r;
    }
    return NULL;
}

static const struct rsdp *find_rsdp(void)
{
    /* BDA word 0x40E holds the EBDA base as a segment; scan its first KiB,
     * then the classic BIOS ROM area.  Both live below 1 MiB (QEMU SeaBIOS
     * and legacy BIOS boot — the GRUB imageiso path goes through the same
     * firmware areas).  An EFI-set-up RSDP would NOT be found here; that
     * costs only the MADT, not the boot (probe fallback). */
    u32 ebda = (u32)bda_word() << 4;
    const struct rsdp *r = NULL;
    if (ebda && ebda < 0xA0000UL)
        r = scan_rsdp(ebda, ebda + 1024);
    if (!r)
        r = scan_rsdp(0xE0000UL, 0x100000UL);
    return r;
}

/* ---- table reads through the bounce window ---------------------------- */

/* Firmware-chosen physical addresses cannot be dereferenced (see the file
 * header): copy through ACPI_SCRATCH_VA instead, one page at a time.
 * vmm_map_kernel_page() invlpgs, so re-pointing the window is safe. */
static void acpi_copy(u64 pa, void *dst0, u32 len)
{
    u8 *dst = dst0;
    while (len) {
        u32 off = (u32)(pa & 0xfffUL);
        u32 n = PAGE_SIZE - off;
        if (n > len) n = len;
        vmm_map_kernel_page(ACPI_SCRATCH_VA, pa & ~0xfffUL, PG_P | PG_W);
        volatile u8 *s = (volatile u8 *)(ACPI_SCRATCH_VA + off);
        for (u32 i = 0; i < n; i++) dst[i] = s[i];
        dst += n;
        pa += n;
        len -= n;
    }
}

/* Copy caps: a directory holds ~1 KiB on any sane firmware and a MADT a
 * few hundred bytes per core.  MAX_CPUS caps what parse_madt() keeps, so
 * a table beyond these caps only ever costs the probe fallback. */
#define ACPI_MAX_DIR  0x2000 /* RSDT/XSDT copy cap */
#define ACPI_MAX_MADT 0x4000 /* MADT copy cap */

static u8 s_dir[ACPI_MAX_DIR];   /* the RSDT or XSDT, as copied */
static u8 s_madt[ACPI_MAX_MADT]; /* the MADT, as copied */

/* Copy the table at `pa` into `dst` (capacity `cap`) and validate it the
 * way the old in-place sdt_ok() did: signature, sane length, checksum
 * over exactly that many bytes.  Returns the validated length, or 0. */
static u32 read_table(u64 pa, const char *want, u8 *dst, u32 cap)
{
    if (!pa) return 0;
    acpi_copy(pa, dst, sizeof(struct sdt_hdr));
    const struct sdt_hdr *t = (const struct sdt_hdr *)dst;
    if (!sig_is(t->sig, want)) return 0;
    if (t->length < sizeof(struct sdt_hdr) || t->length > cap) return 0;
    if (t->length > sizeof(struct sdt_hdr))
        acpi_copy(pa + sizeof(struct sdt_hdr), dst + sizeof(struct sdt_hdr),
                  t->length - (u32)sizeof(struct sdt_hdr));
    if (!sum_ok(dst, t->length)) return 0;
    return t->length;
}

/* Walk XSDT (preferred, 64-bit entries) or RSDT for the MADT.  On success
 * the MADT is fully copied into s_madt (parse_madt() and everything it
 * hands out point into that buffer) and its physical address is returned
 * for the boot log.  The directory is copied once into s_dir and walked
 * from there, so an MADT that fails validation can be skipped and the
 * walk resumed. */
static u64 find_madt(const struct rsdp *r)
{
    /* Rev-2+ firmware also carries the XSDT.  Use it when the RSDP's own
     * extended checksum holds up, else fall back to the RSDT; a broken
     * field in one path must not lose the other. */
    if (r->revision >= 2 && r->xsdt && r->length >= sizeof(struct rsdp) &&
        r->length <= 0x1000 && sum_ok(r, r->length)) {
        if (read_table(r->xsdt, "XSDT", s_dir, sizeof(s_dir))) {
            const struct sdt_hdr *d = (const struct sdt_hdr *)s_dir;
            u32 n = (d->length - (u32)sizeof(struct sdt_hdr)) / 8;
            const u64 *e = (const u64 *)(s_dir + sizeof(struct sdt_hdr));
            for (u32 i = 0; i < n; i++)
                if (read_table(e[i], "APIC", s_madt, sizeof(s_madt)))
                    return e[i];
        }
    }
    if (r->rsdt) {
        if (read_table(r->rsdt, "RSDT", s_dir, sizeof(s_dir))) {
            const struct sdt_hdr *d = (const struct sdt_hdr *)s_dir;
            u32 n = (d->length - (u32)sizeof(struct sdt_hdr)) / 4;
            const u32 *e = (const u32 *)(s_dir + sizeof(struct sdt_hdr));
            for (u32 i = 0; i < n; i++)
                if (read_table(e[i], "APIC", s_madt, sizeof(s_madt)))
                    return e[i];
        }
    }
    return 0;
}

/* ---- MADT ---- */

static bool ap_id_seen(u32 id)
{
    for (u32 i = 0; i < s_ap_count; i++)
        if (s_ap_ids[i] == id) return true;
    return false;
}

static void parse_madt(const struct sdt_hdr *madt)
{
    const u8 *p = (const u8 *)madt;
    u32 total = madt->length;
    if (total < MADT_BODY_MIN) return;

    /* p+36: LAPIC MMIO address, p+40: flags.  The address itself is
     * ignored on purpose: the SDM makes MSR_IA32_APIC_BASE authoritative,
     * and apic_init() reads it from there. */

    for (u32 off = MADT_BODY_MIN; off + 2 <= total; ) {
        u8 type = p[off], len = p[off + 1];
        if (len < 2 || off + len > total) break; /* malformed: stop here */

        if (type == 0 && len >= 8) {
            /* Processor Local APIC: id u8 @3, flags u32 @4 */
            u32 id = p[off + 3], flags = rd32(p + off + 4);
            /* bit0 = enabled; bit1 = "online capable" (ACPI 6.3 uses it for
             * processors the firmware hasn't enabled yet but hands to the
             * OS) — take either, like Linux's MADT_APIC_SAPIC_ENABLED check
             * over both bits. */
            if ((flags & 3) && !ap_id_seen(id) && s_ap_count < MAX_CPUS)
                s_ap_ids[s_ap_count++] = id;
        } else if (type == 9 && len >= 16) {
            /* Processor Local x2APIC: id u32 @4, flags u32 @8 */
            u32 id = rd32(p + off + 4), flags = rd32(p + off + 8);
            if ((flags & 3) && !ap_id_seen(id) && s_ap_count < MAX_CPUS)
                s_ap_ids[s_ap_count++] = id;
        } else if (type == 1 && len >= 12 && !s_ioapic_base) {
            /* IOAPIC: address u32 @4 — first one wins (the kernel routes
             * all IRQs through a single IOAPIC anyway). */
            s_ioapic_base = rd32(p + off + 4);
        }
        off += len;
    }
}

void acpi_init(void)
{
    const struct rsdp *r = find_rsdp();
    if (!r) {
        kprintf("[acpi] RSDP not found — AP enumeration will probe APIC IDs\n");
        return;
    }
    u64 madt_pa = find_madt(r);
    if (!madt_pa) {
        kprintf("[acpi] RSDP %p rev %u, no MADT — AP enumeration will probe\n",
                (void *)r, r->revision);
        return;
    }
    parse_madt((const struct sdt_hdr *)s_madt);
    kprintf("[acpi] RSDP %p rev %u, MADT @ %p: %u local APIC%s, IOAPIC @ %p\n",
            (void *)r, r->revision, (void *)madt_pa, s_ap_count,
            s_ap_count >= MAX_CPUS ? "s (capped)" : "s",
            (void *)(u64)s_ioapic_base);
}

u32 acpi_collect_ap_ids(u32 *ids, u32 max)
{
    u32 n = s_ap_count < max ? s_ap_count : max;
    for (u32 i = 0; i < n; i++) ids[i] = s_ap_ids[i];
    return n;
}

u32 acpi_ioapic_base(void)
{
    return s_ioapic_base;
}
