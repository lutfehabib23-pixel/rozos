/* rozOS — stty [rows cols | auto]: কনসোলের মাপ দেখা/বদলানো
 *
 * serial টার্মিনালের মাপ স্ক্রিনের (ফ্রেমবাফার) থেকে আলাদা হলে এডিটর ঠিকমতো আঁকার জন্য:
 *   stty 24 80     (Linux/WSL টার্মিনালে `stty size` দিয়ে মাপ জানা যায়)
 *   stty auto      আবার স্ক্রিনের মাপ
 *   stty detect    ধাপ ১২: serial টার্মিনালকে জিজ্ঞেস করে মাপ (ESC[6n) — edit/help নিজেরাই করে */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    struct winsize ws;
    if (argc == 2 && !strcmp(argv[1], "detect")) {
        if (ioctl(0, TIOCROZDETECT_FORCE, &ws) < 0) { perror("stty"); return 1; }
        printf("rows %u, columns %u (asked the terminal)\n", ws.ws_row, ws.ws_col);
        return 0;
    }
    if (argc == 3 || (argc == 2 && !strcmp(argv[1], "auto"))) {
        memset(&ws, 0, sizeof(ws));
        if (argc == 3) {
            ws.ws_row = (unsigned short)atoi(argv[1]);
            ws.ws_col = (unsigned short)atoi(argv[2]);
            if (ws.ws_row < 5 || ws.ws_col < 20) { fprintf(stderr, "stty: too small\n"); return 1; }
        }
        if (ioctl(0, TIOCSWINSZ, &ws) < 0) { perror("stty"); return 1; }
    } else if (argc != 1) {
        fprintf(stderr, "usage: stty [<rows> <cols> | auto | detect]\n");
        return 2;
    }
    if (ioctl(0, TIOCGWINSZ, &ws) < 0) { perror("stty"); return 1; }
    printf("rows %u, columns %u\n", ws.ws_row, ws.ws_col);
    return 0;
}
