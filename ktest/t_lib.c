/* EARLY self-tests: the freestanding C library, the ANSI tokenizer and the
 * address-arithmetic macros.  Nothing here allocates, so it can run before
 * pmm_init() -- everything lives on the boot stack. */
#include <sys/ktest.h>
#include <console.h>

static void test_mem(void)
{
    u8 a[32], b[32];

    K_EXPECT(memset(a, 0x5A, sizeof(a)) == a);
    K_EXPECT_EQ(a[0], 0x5A);
    K_EXPECT_EQ(a[31], 0x5A);

    memset(a + 4, 0, 8);
    K_EXPECT_EQ(a[3], 0x5A);
    K_EXPECT_EQ(a[4], 0);
    K_EXPECT_EQ(a[11], 0);
    K_EXPECT_EQ(a[12], 0x5A);

    memcpy(b, a, sizeof(a));
    K_EXPECT_EQ(memcmp(a, b, sizeof(a)), 0);
    b[7] ^= 1;
    K_EXPECT(memcmp(a, b, sizeof(a)) != 0);

    /* memcmp orders by unsigned byte value (0x80 > 0x01) */
    u8 hi = 0x80, lo = 0x01;
    K_EXPECT(memcmp(&hi, &lo, 1) > 0);
    K_EXPECT(memcmp(&lo, &hi, 1) < 0);
    K_EXPECT_EQ(memcmp(a, a, sizeof(a)), 0);
}

static void test_strings(void)
{
    char buf[16];

    K_EXPECT_EQ(strlen(""), 0);
    K_EXPECT_EQ(strlen("abc"), 3);
    K_EXPECT_EQ(strlen("lnxrm v0.08-TEST"), 16);

    K_EXPECT_EQ(strcmp("abc", "abc"), 0);
    K_EXPECT(strcmp("abc", "abd") < 0);
    K_EXPECT(strcmp("abd", "abc") > 0);
    K_EXPECT(strcmp("ab", "abc") < 0); /* prefix sorts first */
    K_EXPECT(strcmp("", "a") < 0);
    /* comparison is unsigned: 0xFF must not sort before 0x01 */
    K_EXPECT(strcmp("\xff", "\x01") > 0);

    K_EXPECT_EQ(strncmp("abcde", "abcxx", 3), 0);
    K_EXPECT(strncmp("abcde", "abcxx", 4) != 0);
    K_EXPECT_EQ(strncmp("abc", "abd", 0), 0);

    K_EXPECT(strcpy(buf, "hey") == buf);
    K_EXPECT_EQ(strcmp(buf, "hey"), 0);

    /* strncpy pads the whole tail with NUL and writes exactly n bytes */
    memset(buf, 0xEE, sizeof(buf));
    K_EXPECT(strncpy(buf, "ab", 6) == buf);
    K_EXPECT_EQ(strcmp(buf, "ab"), 0);
    K_EXPECT_EQ((u8)buf[2], 0);
    K_EXPECT_EQ((u8)buf[5], 0);
    K_EXPECT_EQ((u8)buf[6], 0xEE); /* buf is char[], so widen before comparing */
    K_EXPECT_EQ((u8)buf[7], 0xEE);

    K_EXPECT(strchr("abc", 'b') != NULL);
    K_EXPECT_EQ(*strchr("abc", 'b'), 'b');
    K_EXPECT(strchr("abc", 'z') == NULL);
    K_EXPECT(strchr("abc", '\0') != NULL); /* the terminator is findable */

    K_EXPECT_EQ(strlen(strrchr("a/b/c", '/')), 2); /* "/c" */
    K_EXPECT(strrchr("abc", 'z') == NULL);

    K_EXPECT_EQ(strcasecmp("ABC", "abc"), 0);
    K_EXPECT_EQ(strcasecmp("aBc", "AbC"), 0);
    K_EXPECT(strcasecmp("abc", "abd") < 0);
    K_EXPECT(strcasecmp("ABC", "abc") == 0);
}

static void test_align_macros(void)
{
    K_EXPECT_EQ(ALIGN_UP(0, 4096), 0);
    K_EXPECT_EQ(ALIGN_UP(1, 4096), 4096);
    K_EXPECT_EQ(ALIGN_UP(4095, 4096), 4096);
    K_EXPECT_EQ(ALIGN_UP(4096, 4096), 4096);
    K_EXPECT_EQ(ALIGN_UP(4097, 4096), 8192);
    K_EXPECT_EQ(ALIGN_UP(0x1FFFFF, 0x200000), 0x200000);

    K_EXPECT_EQ(ALIGN_DOWN(0, 4096), 0);
    K_EXPECT_EQ(ALIGN_DOWN(4095, 4096), 0);
    K_EXPECT_EQ(ALIGN_DOWN(4096, 4096), 4096);
    K_EXPECT_EQ(ALIGN_DOWN(4097, 4096), 4096);
    K_EXPECT_EQ(ALIGN_DOWN(0x1FFFFF, 0x200000), 0);

    K_EXPECT_EQ(MIN(3, 7), 3);
    K_EXPECT_EQ(MAX(3, 7), 7);
    K_EXPECT_EQ(MIN(-1, 5), -1); /* signed comparison, then widened */
    K_EXPECT_EQ(MAX(-1, 5), 5);

    /* the high-half identity every PHYS_TO_VIRT() caller depends on */
    K_EXPECT_EQ(PHYS_TO_VIRT(0), 0xffffffff80000000UL);
    K_EXPECT_EQ(PHYS_TO_VIRT(0x100000UL), 0xffffffff80100000UL);
    K_EXPECT_EQ(VIRT_TO_PHYS(0xffffffff80100000UL), 0x100000UL);
    K_EXPECT_EQ(VIRT_TO_PHYS(PHYS_TO_VIRT(0x3F00000UL)), 0x3F00000UL);
}

/* The tokenizer that both console backends share: a wrong phase transition
 * garbles every colour and cursor sequence the kernel prints. */
static int feed_all(struct ansi_seq *s, const char *seq, char *out, int outcap)
{
    int done = 0;
    memset(s, 0, sizeof(*s));
    for (const char *p = seq; *p; p++) {
        int r = ansi_feed(s, *p);
        if (r == ANSI_DONE) {
            if (done < outcap) {
                strncpy(out + done * (ANSI_SEQ_MAX + 1), s->buf, ANSI_SEQ_MAX);
                out[done * (ANSI_SEQ_MAX + 1) + ANSI_SEQ_MAX] = 0;
            }
            done++;
        }
    }
    return done;
}

static void test_ansi_tokenizer(void)
{
    struct ansi_seq s;
    char got[3 * (ANSI_SEQ_MAX + 1)];

    /* set-fg-colour: ESC [ 3 1 m */
    memset(got, 0, sizeof(got));
    K_EXPECT_EQ(feed_all(&s, "\033[31m", got, 3), 1);
    K_EXPECT(strcmp(got, "31m") == 0);
    K_EXPECT_EQ(s.phase, 0);      /* back to plain text */
    K_EXPECT_EQ(ansi_feed(&s, 'A'), ANSI_TEXT);

    /* clear screen: ESC [ 2 J */
    memset(got, 0, sizeof(got));
    K_EXPECT_EQ(feed_all(&s, "\033[2J", got, 3), 1);
    K_EXPECT(strcmp(got, "2J") == 0);

    /* cursor position with two parameters: ESC [ 1 0 ; 2 0 H */
    memset(got, 0, sizeof(got));
    K_EXPECT_EQ(feed_all(&s, "\033[10;20H", got, 3), 1);
    K_EXPECT(strcmp(got, "10;20H") == 0);

    /* two sequences in a row must both be reported */
    memset(got, 0, sizeof(got));
    K_EXPECT_EQ(feed_all(&s, "\033[0m\033[32m", got, 3), 2);
    K_EXPECT(strcmp(got, "0m") == 0);
    K_EXPECT(strcmp(got + ANSI_SEQ_MAX + 1, "32m") == 0);

    /* plain text never enters the machine */
    K_EXPECT_EQ(feed_all(&s, "hello", got, 3), 0);
    K_EXPECT_EQ(ansi_feed(&s, 'x'), ANSI_TEXT);

    /* ESC followed by something other than '[' cancels, not hangs */
    memset(&s, 0, sizeof(s));
    K_EXPECT_EQ(ansi_feed(&s, '\033'), ANSI_CONSUMED);
    K_EXPECT_EQ(s.phase, 1);
    K_EXPECT_EQ(ansi_feed(&s, 'c'), ANSI_CONSUMED);
    K_EXPECT_EQ(s.phase, 0);
    K_EXPECT_EQ(ansi_feed(&s, 'x'), ANSI_TEXT);

    /* palette index table: 30..37 -> VGA attribute nibble */
    K_EXPECT_EQ(ansi_color_index(30), 0);
    K_EXPECT_EQ(ansi_color_index(31), 4);
    K_EXPECT_EQ(ansi_color_index(32), 2);
    K_EXPECT_EQ(ansi_color_index(33), 6);
    K_EXPECT_EQ(ansi_color_index(34), 1);
    K_EXPECT_EQ(ansi_color_index(35), 5);
    K_EXPECT_EQ(ansi_color_index(36), 3);
    K_EXPECT_EQ(ansi_color_index(37), 7);
}

/* An unterminated or over-long sequence must never write past s->buf; the
 * bytes sitting after the struct are the canary that proves it didn't. */
static void test_ansi_truncation(void)
{
    struct wrap {
        struct ansi_seq s;
        u8 tail[16];
    } w;
    size_t canary;

    memset(&w, 0xAA, sizeof(w));
    memset(&w.s, 0, sizeof(w.s));

    /* ESC [ with no terminator: phase 2 forever, nothing reported, and a
     * parameter list far longer than the buffer is dropped, not copied */
    K_EXPECT_EQ(ansi_feed(&w.s, '\033'), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, '['), ANSI_CONSUMED);
    for (int i = 0; i < 64; i++) ansi_feed(&w.s, '3');
    K_EXPECT_EQ(w.s.phase, 2);
    K_EXPECT_EQ(w.s.len, ANSI_SEQ_MAX - 1); /* saturated, not overrun */
    canary = 0;
    for (int i = 0; i < 16; i++)
        if (w.tail[i] != 0xAA) canary++;
    K_EXPECT_EQ(canary, 0);

    /* the final byte completes the sequence -- params only, the terminator
     * itself fell outside the buffer, which is the documented truncation */
    K_EXPECT_EQ(ansi_feed(&w.s, 'm'), ANSI_DONE);
    K_EXPECT_EQ(w.s.phase, 0);
    K_EXPECT(strcmp(w.s.buf, "3333333") == 0);
    canary = 0;
    for (int i = 0; i < 16; i++)
        if (w.tail[i] != 0xAA) canary++;
    K_EXPECT_EQ(canary, 0);

    /* a half-finished sequence swallows text until a letter shows up --
     * that is what makes an un-terminated ESC eat the next word */
    memset(&w.s, 0, sizeof(w.s));
    K_EXPECT_EQ(ansi_feed(&w.s, '\033'), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, '['), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, '1'), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, '0'), ANSI_CONSUMED); /* digits are params */
    K_EXPECT_EQ(ansi_feed(&w.s, 'H'), ANSI_DONE);
    K_EXPECT(strcmp(w.s.buf, "10H") == 0);
    K_EXPECT_EQ(ansi_feed(&w.s, 'x'), ANSI_TEXT); /* machine is back to normal */

    /* parameter-less and private-mode sequences the console really sends */
    memset(&w.s, 0, sizeof(w.s));
    K_EXPECT_EQ(ansi_feed(&w.s, '\033'), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, '['), ANSI_CONSUMED);
    K_EXPECT_EQ(ansi_feed(&w.s, 'm'), ANSI_DONE);
    K_EXPECT(strcmp(w.s.buf, "m") == 0);

    char got[2 * (ANSI_SEQ_MAX + 1)];
    memset(got, 0, sizeof(got));
    K_EXPECT_EQ(feed_all(&w.s, "\033[?25l", got, 2), 1);
    K_EXPECT(strcmp(got, "?25l") == 0);
}

/* The assertion macros themselves (include/console.h): a passing ASSERT is
 * counted, a WARN is recorded without failing the run, and BUG_ON(0) is
 * inert.  A *failing* ASSERT cannot be exercised here -- it would stop the
 * boot by design -- so that path is verified by injecting one by hand. */
static void test_assert_macros(void)
{
    unsigned p0 = selftest_pass, f0 = selftest_fail, w0 = selftest_warn;

    ASSERT(1 + 1 == 2, "arithmetic still works");
    ASSERT(strcmp("a", "a") == 0, "strcmp(%s,%s) == 0", "a", "a");
    K_EXPECT(selftest_pass == p0 + 2);
    K_EXPECT_EQ(selftest_fail, f0);

    WARN(1, "not taken: this must not even print");
    K_EXPECT_EQ(selftest_warn, w0);
    WARN(0, "deliberate warning from the self-test: a warning, not a failure");
    K_EXPECT_EQ(selftest_warn, w0 + 1);
    K_EXPECT_EQ(selftest_fail, f0); /* warnings never fail the run */

    BUG_ON(0); /* inert; a firing BUG_ON halts the machine by contract */
    K_EXPECT_EQ(selftest_fail, f0);
}

KTEST("lib", KTEST_EARLY, test_mem);
KTEST("lib", KTEST_EARLY, test_strings);
KTEST("lib", KTEST_EARLY, test_align_macros);
KTEST("console", KTEST_EARLY, test_ansi_tokenizer);
KTEST("console", KTEST_EARLY, test_ansi_truncation);
KTEST("ktest", KTEST_EARLY, test_assert_macros);
