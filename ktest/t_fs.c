/* Pure string work on the filesystem side: path normalisation and the 8.3
 * name codec.  Both fail silently -- a wrong abs_path looks up a different
 * file, a wrong short name makes a file vanish from `ls`, and neither ever
 * returns an error anyone would notice. */
#include <sys/ktest.h>
#include <console.h>
#include <sys/vfs.h>
#include <fat32_priv.h>

static void test_abs_path(void)
{
    char buf[32];
    const char *r;

    /* relative -> '/' + path, written into buf and terminated */
    memset(buf, 0xEE, sizeof(buf));
    r = vfs_abs_path("bin/init", buf, sizeof(buf));
    K_EXPECT(r == buf);
    K_EXPECT_EQ(strcmp(r, "/bin/init"), 0);
    K_EXPECT_EQ((u8)buf[8], 't');
    K_EXPECT_EQ((u8)buf[9], 0);     /* terminator right after the text */
    K_EXPECT_EQ((u8)buf[10], 0xEE); /* and nothing beyond it was touched */

    /* empty path still yields a usable root */
    memset(buf, 0xEE, sizeof(buf));
    r = vfs_abs_path("", buf, sizeof(buf));
    K_EXPECT(r == buf);
    K_EXPECT_EQ(strcmp(r, "/"), 0);
    K_EXPECT_EQ((u8)buf[1], 0);
    K_EXPECT_EQ((u8)buf[2], 0xEE);

    /* absolute path passes through: same pointer, buf stays untouched */
    const char *given = "/usr/bin/sh";
    memset(buf, 0xEE, sizeof(buf));
    r = vfs_abs_path(given, buf, sizeof(buf));
    K_EXPECT(r == given);
    K_EXPECT_EQ((u8)buf[0], 0xEE);

    /* overlong: refused with NULL, never cut short (C46).  A truncated
     * path is a different path, so the caller answers LNXRM_ENAMETOOLONG
     * instead of letting the kernel look up a file nobody named.  buf
     * must not be left holding a plausible-looking shorter path and the
     * byte past the terminator is never written. */
    char longpath[128];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = 0;
    memset(buf, 0xEE, sizeof(buf));
    r = vfs_abs_path(longpath, buf, sizeof(buf));
    K_EXPECT(r == NULL);
    K_EXPECT_EQ((u8)buf[sizeof(buf) - 1], 0xEE); /* final byte never written */

    /* the exact boundary: '/' + 29 payload chars + NUL fills buf[32] to
     * its last byte and still fits; one payload char more is refused */
    char near[32];
    memset(near, 'b', 29);
    near[29] = 0; /* 29 chars: fits */
    memset(buf, 0xEE, sizeof(buf));
    r = vfs_abs_path(near, buf, sizeof(buf));
    K_EXPECT(r == buf);
    K_EXPECT_EQ(strlen(buf), 30);    /* '/' + 29 */
    K_EXPECT_EQ((u8)buf[30], 0);     /* terminator in the last usable slot */
    K_EXPECT_EQ((u8)buf[31], 0xEE);  /* final byte survives */

    near[29] = 'b'; /* 30 chars: one too many */
    near[30] = 0;
    r = vfs_abs_path(near, buf, sizeof(buf));
    K_EXPECT(r == NULL);

    /* a buffer too small to hold "/" is refused and not written at all */
    const char *rel = "rel";
    char tiny[1] = {'!'};
    r = vfs_abs_path(rel, tiny, sizeof(tiny));
    K_EXPECT(r == NULL);
    K_EXPECT_EQ(tiny[0], '!');
}

/* 8.3 -> display name.  The helper asserts the inputs really are 11 bytes,
 * because a miscounted literal would otherwise test nothing. */
static void to_name(const char *short11, u8 ntres, char *out)
{
    K_EXPECT_EQ(strlen(short11), 11);
    fat_short_to_name((const u8 *)short11, ntres, out);
}

static void test_fat_short_to_name(void)
{
    struct { /* the documented output is 13 bytes: catch an overrun */
        char name[13];
        u8 tail[4];
    } out;

    /* Case comes from the NT bits, never from a fixed tolower(): both bits
     * set lowercases both halves ... */
    memset(&out, 0xEE, sizeof(out));
    to_name("README  TXT", FAT_NTRES_LOWER_BASE | FAT_NTRES_LOWER_EXT, out.name);
    K_EXPECT_EQ(strcmp(out.name, "readme.txt"), 0);
    K_EXPECT_EQ(out.tail[0], 0xEE);

    memset(&out, 0xEE, sizeof(out));
    to_name("ABCDEFGHTXT", 0, out.name); /* 8 + 3: the longest legal name */
    K_EXPECT_EQ(strcmp(out.name, "ABCDEFGH.TXT"), 0);
    K_EXPECT_EQ(out.tail[0], 0xEE);

    memset(&out, 0xEE, sizeof(out));
    to_name("           ", 0, out.name); /* all 11 spaces: empty name */
    K_EXPECT_EQ(out.name[0], 0);
    K_EXPECT_EQ(out.tail[0], 0xEE);

    memset(&out, 0xEE, sizeof(out));
    to_name("NAME       ", FAT_NTRES_LOWER_BASE, out.name); /* base only */
    K_EXPECT_EQ(strcmp(out.name, "name"), 0);
    K_EXPECT_EQ(out.tail[0], 0xEE);

    memset(&out, 0xEE, sizeof(out));
    to_name("A.B     TXT", FAT_NTRES_LOWER_BASE | FAT_NTRES_LOWER_EXT,
            out.name); /* a dot inside the base is not a separator */
    K_EXPECT_EQ(strcmp(out.name, "a.b.txt"), 0);

    memset(&out, 0xEE, sizeof(out));
    to_name("NAME    T  ", FAT_NTRES_LOWER_BASE | FAT_NTRES_LOWER_EXT,
            out.name); /* one-character extension, space padded */
    K_EXPECT_EQ(strcmp(out.name, "name.t"), 0);

    /* ... and a bit that is clear leaves the byte exactly as stored, which is
     * how the root directory's "README  MD " + LOWER_EXT reads back as
     * "README.md" instead of the old, unconditional "readme.md". */
    memset(&out, 0xEE, sizeof(out));
    to_name("README  MD ", FAT_NTRES_LOWER_EXT, out.name);
    K_EXPECT_EQ(strcmp(out.name, "README.md"), 0);
    K_EXPECT_EQ(out.tail[0], 0xEE);
}

static void test_fat_name_eq_short(void)
{
    struct fat_dirent e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, "HELLO   TXT", 11);

    /* case never matters in either direction */
    K_EXPECT(fat_name_eq_short("hello.txt", &e));
    K_EXPECT(fat_name_eq_short("HELLO.TXT", &e));
    K_EXPECT(fat_name_eq_short("Hello.Txt", &e));

    /* a wrong extension or a wrong stem is a miss */
    K_EXPECT(!fat_name_eq_short("hello.tx", &e));
    K_EXPECT(!fat_name_eq_short("hell.txt", &e));
    K_EXPECT(!fat_name_eq_short("hello", &e)); /* no extension at all */

    /* 8.3 truncation is the documented contract: name_to_short() stores
     * only the first three extension characters, so a longer extension
     * still matches the stored short name */
    K_EXPECT(fat_name_eq_short("hello.txt2", &e));
}

KTEST("vfs", KTEST_LATE, test_abs_path);
KTEST("fat32", KTEST_LATE, test_fat_short_to_name);
KTEST("fat32", KTEST_LATE, test_fat_name_eq_short);
