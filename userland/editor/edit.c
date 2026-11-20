/* rozOS — edit: nano-ধাঁচের সহজ টেক্সট এডিটর (ধাপ ৭)
 *
 *   edit [file]
 *
 * কী:
 *   তীর, Home/End, PgUp/PgDn  কার্সর সরানো        Ctrl+S  সংরক্ষণ
 *   Ctrl+Q বা Ctrl+X         বের হওয়া            Ctrl+F  খোঁজা (আবার Ctrl+F = পরেরটা)
 *   Ctrl+G                   লাইনে যাওয়া         Ctrl+K  লাইন কাটা, Ctrl+U  বসানো
 *   Backspace/Delete, Enter, Tab  যেমন হওয়ার কথা
 *
 * স্ক্রিন ANSI escape দিয়ে আঁকা হয় (কার্নেলের tty আর serial টার্মিনাল দুটোই বোঝে)।
 * প্রতিবার শুধু বদলে যাওয়া লাইনগুলো পাঠানো হয় — serial-এ ধীর লিংকেও দ্রুত।
 * স্ক্রিনের মাপ: TIOCGWINSZ (shell-এ `stty <rows> <cols>` দিয়ে বদলানো যায়) বা LINES/COLUMNS।
 * ফাইল লাইনের তালিকা হিসেবে মেমোরিতে থাকে; সংরক্ষণে পুরো ফাইল নতুন করে লেখা হয়। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define TAB_STOP   4
#define MSG_MAX    160
#define CTRL(k)    ((k) & 0x1F)

enum {
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC
};

struct line {
    char *s;
    int len, cap;
};

static struct line *lines;
static int nlines, lines_cap;
static int cx, cy;                     /* কার্সর: লাইনের ভেতরে বাইট, লাইন নম্বর */
static int rowoff, coloff;             /* স্ক্রল */
static int screen_rows, screen_cols;   /* পুরো স্ক্রিন */
static int text_rows;                  /* লেখার জায়গা (উপরে শিরোনাম, নিচে ২ লাইন) */
static char filename[1024];
static int modified;
static char message[MSG_MAX];
static char **frame;                   /* আগের বার আঁকা লাইনগুলো (পরিবর্তন খোঁজার জন্য) */
static char *clip;                     /* Ctrl+K দিয়ে কাটা লাইন */
static int clip_len;
static char last_search[128];

/* ---------- আউটপুট বাফার ---------- */

static char *obuf;
static size_t olen, ocap;

static void oput(const char *s, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2 + 1024;
        obuf = (char *)realloc(obuf, ocap);
        if (!obuf) exit(1);
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void ostr(const char *s) { oput(s, strlen(s)); }

static void oflush(void)
{
    size_t done = 0;
    while (done < olen) {
        ssize_t n = write(1, obuf + done, olen - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    olen = 0;
}

/* ---------- লাইন ---------- */

static void line_reserve(struct line *l, int need)
{
    if (need + 1 > l->cap) {
        l->cap = need * 2 + 16;
        l->s = (char *)realloc(l->s, (size_t)l->cap);
        if (!l->s) exit(1);
    }
}

static void insert_line(int at, const char *s, int len)
{
    if (nlines == lines_cap) {
        lines_cap = lines_cap ? lines_cap * 2 : 64;
        lines = (struct line *)realloc(lines, sizeof(*lines) * (size_t)lines_cap);
        if (!lines) exit(1);
    }
    memmove(&lines[at + 1], &lines[at], sizeof(*lines) * (size_t)(nlines - at));
    lines[at].s = NULL;
    lines[at].len = 0;
    lines[at].cap = 0;
    line_reserve(&lines[at], len);
    memcpy(lines[at].s, s, (size_t)len);
    lines[at].len = len;
    lines[at].s[len] = '\0';
    nlines++;
}

static void delete_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], sizeof(*lines) * (size_t)(nlines - at - 1));
    nlines--;
}

/* ---------- ফাইল ---------- */

static void set_message(const char *fmt, const char *arg)
{
    snprintf(message, sizeof(message), fmt, arg);
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;

    if (!f) {
        if (errno == ENOENT) {
            insert_line(0, "", 0);
            set_message("New file: %s", path);
            return 0;
        }
        return -1;
    }
    while ((n = getline(&buf, &cap, f)) >= 0) {
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        insert_line(nlines, buf, (int)n);
    }
    free(buf);
    fclose(f);
    if (nlines == 0) insert_line(0, "", 0);
    snprintf(message, sizeof(message), "Read %d line%s", nlines, nlines == 1 ? "" : "s");
    return 0;
}

static int save_file(void)
{
    int fd, i;
    long total = 0;
    if (!filename[0]) {
        set_message("%s", "No file name - start with: edit <file>");
        return -1;
    }
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(message, sizeof(message), "Cannot save %s: %s", filename, strerror(errno));
        return -1;
    }
    olen = 0;
    for (i = 0; i < nlines; i++) {
        oput(lines[i].s, (size_t)lines[i].len);
        oput("\n", 1);
    }
    total = (long)olen;
    {
        size_t done = 0;
        while (done < olen) {
            ssize_t n = write(fd, obuf + done, olen - done);
            if (n <= 0) {
                close(fd);
                olen = 0;
                snprintf(message, sizeof(message), "Write error: %s", strerror(errno));
                return -1;
            }
            done += (size_t)n;
        }
    }
    olen = 0;
    close(fd);
    modified = 0;
    snprintf(message, sizeof(message), "Saved %s (%d lines, %ld bytes)", filename, nlines, total);
    return 0;
}

/* ---------- কী পড়া ---------- */

static int read_byte(void)
{
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

static int read_key(void)
{
    int c = read_byte(), num = 0;
    if (c != 0x1B) return c;
    c = read_byte();
    if (c != '[' && c != 'O') return K_ESC;
    for (;;) {
        c = read_byte();
        if (c < 0) return K_ESC;
        if (isdigit(c)) { num = num * 10 + (c - '0'); continue; }
        if (c == ';') continue;
        break;
    }
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        switch (num) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return K_DEL;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        }
    }
    return K_ESC;
}

/* ---------- কার্সর ও স্ক্রল ---------- */

static int render_x(const struct line *l, int x)
{
    int rx = 0, i;
    for (i = 0; i < x && i < l->len; i++) {
        if (l->s[i] == '\t') rx += TAB_STOP - (rx % TAB_STOP);
        else rx++;
    }
    return rx;
}

static void clamp_cursor(void)
{
    if (cy >= nlines) cy = nlines - 1;
    if (cy < 0) cy = 0;
    if (cx > lines[cy].len) cx = lines[cy].len;
    if (cx < 0) cx = 0;
}

static void scroll(void)
{
    int rx = render_x(&lines[cy], cx);
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + text_rows) rowoff = cy - text_rows + 1;
    if (rx < coloff) coloff = rx;
    if (rx >= coloff + screen_cols - 1) coloff = rx - screen_cols + 2;
}

/* ---------- আঁকা ---------- */

/* একটা লাইনের দেখার রূপ (tab → স্পেস), coloff থেকে width অক্ষর */
static void render_line(const struct line *l, char *out, int width)
{
    int rx = 0, n = 0, i;
    for (i = 0; i < l->len && n < width; i++) {
        unsigned char c = (unsigned char)l->s[i];
        int k = 1, j;
        char ch = (char)c;
        if (c == '\t') { k = TAB_STOP - (rx % TAB_STOP); ch = ' '; }
        else if (c < 32 || c >= 127) ch = '?';
        for (j = 0; j < k; j++, rx++) {
            if (rx >= coloff && n < width) out[n++] = ch;
        }
    }
    out[n] = '\0';
}

/* row-তম স্ক্রিন-লাইনের পুরো লেখা (ANSI সহ) বানিয়ে, আগের থেকে আলাদা হলে পাঠানো */
static void put_row(int row, const char *content)
{
    char pos[24];
    if (frame[row] && !strcmp(frame[row], content)) return;
    free(frame[row]);
    frame[row] = strdup(content);
    snprintf(pos, sizeof(pos), "\x1b[%d;1H", row + 1);
    ostr(pos);
    ostr(content);
    ostr("\x1b[K");
}

static void draw(void)
{
    char *buf = (char *)malloc((size_t)screen_cols * 4 + 64);
    char *txt = (char *)malloc((size_t)screen_cols + 1);
    int r, w = screen_cols - 1;                  /* শেষ কলামে লিখলে কিছু টার্মিনাল লাইন বদলায় */
    char pos[32];

    if (!buf || !txt) exit(1);
    clamp_cursor();
    scroll();
    ostr("\x1b[?25l");

    /* শিরোনাম */
    snprintf(txt, (size_t)screen_cols, "  edit  %s%s", filename[0] ? filename : "[no name]",
             modified ? "  [modified]" : "");
    snprintf(buf, (size_t)screen_cols * 4 + 64, "\x1b[7m%-*.*s\x1b[0m", w, w, txt);
    put_row(0, buf);

    for (r = 0; r < text_rows; r++) {
        int i = rowoff + r;
        if (i < nlines) {
            render_line(&lines[i], txt, w);
            put_row(r + 1, txt);
        } else {
            put_row(r + 1, "\x1b[34m~\x1b[0m");
        }
    }

    /* অবস্থা-লাইন ও সাহায্য */
    snprintf(txt, (size_t)screen_cols, "%s", message);
    snprintf(buf, (size_t)screen_cols * 4 + 64, "%-*.*s", w - 18, w - 18, txt);
    {
        char where[40];
        snprintf(where, sizeof(where), "%6d:%-4d %3d%%", cy + 1, cx + 1,
                 nlines > 1 ? cy * 100 / (nlines - 1) : 100);
        strncat(buf, where, (size_t)screen_cols);
    }
    put_row(screen_rows - 2, buf);
    snprintf(txt, (size_t)screen_cols, "^S Save  ^Q Quit  ^F Find  ^G Go to line  ^K Cut line  ^U Paste");
    snprintf(buf, (size_t)screen_cols * 4 + 64, "\x1b[7m%.*s\x1b[0m", w, txt);
    put_row(screen_rows - 1, buf);

    snprintf(pos, sizeof(pos), "\x1b[%d;%dH\x1b[?25h", cy - rowoff + 2,
             render_x(&lines[cy], cx) - coloff + 1);
    ostr(pos);
    oflush();
    free(buf);
    free(txt);
}

static void full_redraw(void)
{
    int r;
    for (r = 0; r < screen_rows; r++) {
        free(frame[r]);
        frame[r] = NULL;
    }
    ostr("\x1b[0m\x1b[2J");
    draw();
}

/* নিচের অবস্থা-লাইনে প্রশ্ন করে উত্তর পড়া; Esc/Ctrl+C = বাতিল (-1) */
static int prompt_input(const char *question, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (;;) {
        int k;
        snprintf(message, sizeof(message), "%s%s", question, out);
        draw();
        k = read_key();
        if (k == '\r' || k == '\n') return (int)n;
        if (k == K_ESC || k == CTRL('c') || k == CTRL('q')) {
            message[0] = '\0';
            return -1;
        }
        if ((k == '\b' || k == 127) && n > 0) out[--n] = '\0';
        else if (k >= 32 && k < 127 && n + 1 < size) { out[n++] = (char)k; out[n] = '\0'; }
    }
}

/* ---------- সম্পাদনা ---------- */

static void insert_char(int c)
{
    struct line *l = &lines[cy];
    line_reserve(l, l->len + 1);
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
    l->s[cx++] = (char)c;
    l->len++;
    modified = 1;
}

static void insert_newline(void)
{
    struct line *l = &lines[cy];
    int indent = 0;
    /* আগের লাইনের শুরুর ফাঁকা অংশ নতুন লাইনেও (কোড লেখায় সুবিধা) */
    while (indent < cx && (l->s[indent] == ' ' || l->s[indent] == '\t')) indent++;
    insert_line(cy + 1, l->s + cx, l->len - cx);
    l = &lines[cy];                                 /* realloc হয়ে থাকতে পারে */
    l->len = cx;
    l->s[cx] = '\0';
    if (indent) {
        struct line *n = &lines[cy + 1];
        line_reserve(n, n->len + indent);
        memmove(n->s + indent, n->s, (size_t)n->len + 1);
        memcpy(n->s, l->s, (size_t)indent);
        n->len += indent;
    }
    cy++;
    cx = indent;
    modified = 1;
}

static void backspace(void)
{
    if (cx > 0) {
        struct line *l = &lines[cy];
        memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
        l->len--;
        cx--;
        modified = 1;
    } else if (cy > 0) {
        struct line *prev = &lines[cy - 1], *cur = &lines[cy];
        int plen = prev->len;
        line_reserve(prev, prev->len + cur->len);
        memcpy(prev->s + prev->len, cur->s, (size_t)cur->len + 1);
        prev->len += cur->len;
        delete_line(cy);
        cy--;
        cx = plen;
        modified = 1;
    }
}

static void delete_char(void)
{
    if (cx < lines[cy].len) {
        cx++;
        backspace();
    } else if (cy + 1 < nlines) {
        cy++;
        cx = 0;
        backspace();
    }
}

static void cut_line(void)
{
    struct line *l = &lines[cy];
    free(clip);
    clip = (char *)malloc((size_t)l->len + 1);
    if (!clip) return;
    memcpy(clip, l->s, (size_t)l->len + 1);
    clip_len = l->len;
    if (nlines > 1) {
        delete_line(cy);
    } else {
        l->len = 0;
        l->s[0] = '\0';
    }
    cx = 0;
    modified = 1;
    set_message("%s", "Cut 1 line (^U to paste)");
}

static void paste_line(void)
{
    if (!clip) {
        set_message("%s", "Nothing to paste - cut a line with ^K first");
        return;
    }
    insert_line(cy, clip, clip_len);
    cy++;
    cx = 0;
    modified = 1;
}

static void find(void)
{
    char q[128];
    int i, start_y = cy, start_x = cx + 1;
    if (prompt_input("Find (Enter = search, Esc = cancel): ", q, sizeof(q)) < 0) return;
    if (q[0]) strcpy(last_search, q);
    if (!last_search[0]) return;
    for (i = 0; i <= nlines; i++) {
        int y = (start_y + i) % nlines;
        int from = (i == 0) ? start_x : 0;
        char *hit;
        if (from > lines[y].len) continue;
        hit = strstr(lines[y].s + from, last_search);
        if (hit) {
            cy = y;
            cx = (int)(hit - lines[y].s);
            set_message("Found \"%s\" (^F again for the next one)", last_search);
            return;
        }
    }
    set_message("\"%s\" not found", last_search);
}

static void goto_line(void)
{
    char q[16];
    int n;
    if (prompt_input("Go to line: ", q, sizeof(q)) <= 0) return;
    n = atoi(q);
    if (n < 1) n = 1;
    if (n > nlines) n = nlines;
    cy = n - 1;
    cx = 0;
    message[0] = '\0';
}

/* ---------- মূল লুপ ---------- */

static void screen_size(void)
{
    struct winsize ws;
    const char *l = getenv("LINES"), *c = getenv("COLUMNS");
    screen_rows = 24;
    screen_cols = 80;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        screen_rows = ws.ws_row;
        screen_cols = ws.ws_col;
    }
    if (l && atoi(l) > 0) screen_rows = atoi(l);
    if (c && atoi(c) > 0) screen_cols = atoi(c);
    if (screen_rows < 5) screen_rows = 5;
    if (screen_cols < 20) screen_cols = 20;
    text_rows = screen_rows - 3;
}

static void leave(int code)
{
    ostr("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    oflush();
    exit(code);
}

int main(int argc, char **argv)
{
    int quit_warned = 0;

    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "edit: needs the console (stdin and stdout)\n");
        return 1;
    }
    screen_size();
    frame = (char **)calloc((size_t)screen_rows, sizeof(char *));
    if (!frame) return 1;
    if (argc > 1) {
        strncpy(filename, argv[1], sizeof(filename) - 1);
        if (load_file(filename) < 0) {
            fprintf(stderr, "edit: %s: %s\n", filename, strerror(errno));
            return 1;
        }
    } else {
        insert_line(0, "", 0);
        set_message("%s", "No file name - text can be viewed but not saved");
    }
    if (!message[0]) set_message("%s", "^S save, ^Q quit");
    full_redraw();

    for (;;) {
        int k = read_key();
        if (k != CTRL('q') && k != CTRL('x')) quit_warned = 0;

        switch (k) {
        case CTRL('q'):
        case CTRL('x'):
            if (modified && !quit_warned) {
                int a;
                /* nano-র মতো: এক কী-তেই উত্তর (Enter লাগে না) */
                set_message("%s", "Save changes before quitting? y = save, n = discard, Esc = cancel");
                draw();
                a = read_key();
                if (a == 'y' || a == 'Y') {
                    if (save_file() < 0) break;
                    leave(0);
                }
                if (a == 'n' || a == 'N') leave(0);
                set_message("%s", "Cancelled");
                break;
            }
            leave(0);
            break;
        case CTRL('s'):
            save_file();
            break;
        case CTRL('f'):
            find();
            break;
        case CTRL('g'):
            goto_line();
            break;
        case CTRL('k'):
            cut_line();
            break;
        case CTRL('u'):
            paste_line();
            break;
        case CTRL('l'):
            screen_size();
            free(frame);
            frame = (char **)calloc((size_t)screen_rows, sizeof(char *));
            if (!frame) leave(1);
            full_redraw();
            continue;
        case '\r':
        case '\n':
            insert_newline();
            break;
        case '\b':
        case 127:
            backspace();
            break;
        case K_DEL:
            delete_char();
            break;
        case K_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = lines[cy].len; }
            break;
        case K_RIGHT:
            if (cx < lines[cy].len) cx++;
            else if (cy + 1 < nlines) { cy++; cx = 0; }
            break;
        case K_UP:
            if (cy > 0) cy--;
            break;
        case K_DOWN:
            if (cy + 1 < nlines) cy++;
            break;
        case K_HOME:
            cx = 0;
            break;
        case K_END:
            cx = lines[cy].len;
            break;
        case K_PGUP:
            cy -= text_rows;
            break;
        case K_PGDN:
            cy += text_rows;
            break;
        case K_ESC:
            break;
        default:
            if (k == '\t' || (k >= 32 && k < 127)) insert_char(k);
            break;
        }
        if (k != CTRL('s') && k != CTRL('f') && k != CTRL('g') && k != CTRL('k') &&
            k != CTRL('u') && k != CTRL('q') && k != CTRL('x') && k < 1000 && k != K_ESC) {
            message[0] = '\0';
        }
        draw();
    }
}
