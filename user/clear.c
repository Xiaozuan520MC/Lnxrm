/* user/clear.c -- 清屏 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ESC[2J  清除整屏
     * ESC[H   光标移到左上角
     * ESC[3J  清除滚动缓冲(可选, 很多终端支持)
     */
    const char *seq = "\x1b[2J\x1b[H\x1b[3J";
    kwrite(1, seq, xstrlen(seq));

    return 0;
}
