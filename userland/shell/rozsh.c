/* rozOS — rozsh: rozOS-এর shell (ধাপ ৭)
 *
 * সুবিধা:
 *   - কমান্ড চালানো: PATH (ডিফল্ট /bin) থেকে খুঁজে fork + execve + wait
 *   - built-in: cd, pwd, exit, history, export, unset, env, echo, help, source (.), time,
 *     true, false, : (ধাপ ১৩), exec (ধাপ ১৪)
 *   - help [topic]: /bin/rozhelp থাকলে লিংকসহ সাহায্য-দর্শক খোলে (ধাপ ৮)
 *   - রিডাইরেকশন: < file, > file, >> file, 2> file, 2>&1; কমান্ড আলাদা করা: ;
 *   - ধাপ ১৩: পাইপ cmd1 | cmd2 | ... (কার্নেলের pipe()), শর্ত cmd1 && cmd2, cmd1 || cmd2
 *   - উদ্ধৃতি: '...', "...", \x; ভেরিয়েবল: $NAME, ${NAME}, $? (শেষ কমান্ডের ফল), $$
 *   - লাইন এডিটিং: ← → Home End Delete Backspace, ↑ ↓ ইতিহাস, Ctrl+U (লাইন মোছা),
 *     Ctrl+W (আগের শব্দ মোছা), Ctrl+L (স্ক্রিন মোছা), Ctrl+C (লাইন বাতিল), Ctrl+D (খালি লাইনে exit)
 *   - ইতিহাস ফাইল: $HOME/.rozsh_history
 *   - স্ক্রিপ্ট: rozsh <file> বা `source file`
 *
 * কনসোল "raw" — কার্নেল নিজে echo বা লাইন-এডিটিং করে না, তাই সব এখানে।
 * তীর-কী আসে ANSI escape sequence হিসেবে (কার্নেল কিবোর্ডের কী-ও সেভাবেই পাঠায়)।
 *   - ধাপ ১৪: job control-এর শুরু — প্রতিটা কমান্ড (বা পুরো পাইপ) নিজের process group-এ,
 *     টার্মিনালের foreground; Ctrl+C সেই গ্রুপকে থামায় (shell নিজে SIGINT উপেক্ষা করে)
 * এখনো নেই (পরে): পেছনে চালানো (&), Tab-সম্পূর্ণকরণ। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <rozos.h>

#define LINE_MAX_LEN 1024
#define MAX_ARGS     256          /* ধাপ ১১: ৬৪ থেকে বাড়ানো (make-এর লম্বা কমান্ড) */
#define HIST_MAX     100
#define WORD_MAX     1024

static char *history[HIST_MAX];
static int hist_count;
static int last_status;
static int interactive;
static int errexit;          /* ধাপ ১১: `set -e` — কোনো কমান্ড ব্যর্থ হলে লাইনের বাকিটা/স্ক্রিপ্ট থামে */
static char hostname[65] = "rozos";
static char prev_dir[LINE_MAX_LEN] = "";

/* ---------- ইতিহাস ---------- */

static void history_path(char *out, size_t size)
{
    const char *home = getenv("HOME");
    snprintf(out, size, "%s/.rozsh_history", home && *home ? home : "/home");
}

static void history_add(const char *line, int save)
{
    if (!*line || (hist_count && !strcmp(history[hist_count - 1], line))) {
        return;
    }
    if (hist_count == HIST_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(history[0]) * (HIST_MAX - 1));
        hist_count--;
    }
    history[hist_count++] = strdup(line);
    if (save) {
        char path[LINE_MAX_LEN];
        int fd;
        history_path(path, sizeof(path));
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            write(fd, line, strlen(line));
            write(fd, "\n", 1);
            close(fd);
        }
    }
}

static void history_load(void)
{
    char path[LINE_MAX_LEN], line[LINE_MAX_LEN];
    FILE *f;
    history_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        history_add(line, 0);
    }
    fclose(f);
}

/* ---------- লাইন এডিটর ---------- */

static void out(const char *s)
{
    write(1, s, strlen(s));
}

static void redraw(const char *prompt, const char *buf, size_t len, size_t pos)
{
    char tmp[32];
    out("\r");
    out(prompt);
    write(1, buf, len);
    out("\x1b[K");
    if (len > pos) {
        snprintf(tmp, sizeof(tmp), "\x1b[%luD", (unsigned long)(len - pos));
        out(tmp);
    }
}

static int read_byte(void)
{
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    return n == 1 ? c : -1;
}

/* ESC-এর পরের অংশ: 'A' 'B' 'C' 'D' 'H' 'F', বা "3~" ইত্যাদি → একটা কোড */
enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_NONE };

static int read_escape(void)
{
    int c = read_byte(), num = 0;
    if (c != '[' && c != 'O') {
        return K_NONE;
    }
    for (;;) {
        c = read_byte();
        if (c < 0) return K_NONE;
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
        if (num == 1 || num == 7) return K_HOME;
        if (num == 4 || num == 8) return K_END;
        if (num == 3) return K_DEL;
        return K_NONE;
    }
    return K_NONE;
}

/* ফেরত: দৈর্ঘ্য, -1 = EOF (Ctrl+D খালি লাইনে) */
static int read_line(const char *prompt, char *buf, size_t size)
{
    size_t len = 0, pos = 0;
    int hist_pos = hist_count;
    char saved[LINE_MAX_LEN] = "";

    if (!interactive) {
        if (!fgets(buf, (int)size, stdin)) return -1;
        buf[strcspn(buf, "\n")] = '\0';
        return (int)strlen(buf);
    }

    out(prompt);
    for (;;) {
        int c = read_byte();
        if (c < 0) return -1;
        if (c == 0x1B) c = read_escape();

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            out("\n");
            return (int)len;
        } else if (c == 4) {                            /* Ctrl+D */
            if (len == 0) { out("\n"); return -1; }
            c = K_DEL;
        } else if (c == 3) {                            /* Ctrl+C */
            out("^C\n");
            len = pos = 0;
            out(prompt);
            continue;
        } else if (c == 12) {                           /* Ctrl+L */
            out("\x1b[2J\x1b[H");
            redraw(prompt, buf, len, pos);
            continue;
        } else if (c == 21) {                           /* Ctrl+U */
            memmove(buf, buf + pos, len - pos);
            len -= pos;
            pos = 0;
            redraw(prompt, buf, len, pos);
            continue;
        } else if (c == 23) {                           /* Ctrl+W */
            size_t p = pos;
            while (p > 0 && buf[p - 1] == ' ') p--;
            while (p > 0 && buf[p - 1] != ' ') p--;
            memmove(buf + p, buf + pos, len - pos);
            len -= pos - p;
            pos = p;
            redraw(prompt, buf, len, pos);
            continue;
        }

        switch (c) {
        case '\b':
        case 127:
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--;
                len--;
                redraw(prompt, buf, len, pos);
            }
            break;
        case K_DEL:
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                len--;
                redraw(prompt, buf, len, pos);
            }
            break;
        case K_LEFT:
            if (pos > 0) { pos--; out("\x1b[D"); }
            break;
        case K_RIGHT:
            if (pos < len) { pos++; out("\x1b[C"); }
            break;
        case K_HOME:
        case 1:                                         /* Ctrl+A */
            pos = 0;
            redraw(prompt, buf, len, pos);
            break;
        case K_END:
        case 5:                                         /* Ctrl+E */
            pos = len;
            redraw(prompt, buf, len, pos);
            break;
        case K_UP:
        case K_DOWN:
            if (c == K_UP && hist_pos > 0) {
                if (hist_pos == hist_count) {
                    memcpy(saved, buf, len);
                    saved[len] = '\0';
                }
                hist_pos--;
            } else if (c == K_DOWN && hist_pos < hist_count) {
                hist_pos++;
            } else {
                break;
            }
            strncpy(buf, hist_pos == hist_count ? saved : history[hist_pos], size - 1);
            buf[size - 1] = '\0';
            len = pos = strlen(buf);
            redraw(prompt, buf, len, pos);
            break;
        default:
            if (c >= 32 && c < 127 && len + 1 < size) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos++] = (char)c;
                len++;
                if (pos == len) {
                    char ch = (char)c;
                    write(1, &ch, 1);
                } else {
                    redraw(prompt, buf, len, pos);
                }
            }
            break;
        }
    }
}

/* ---------- পার্সিং ---------- */

struct redirect {
    int fd;                       /* 0, 1, 2 */
    int flags;                    /* open-এর */
    char *path;                   /* NULL হলে dup_from */
    int dup_from;
};

/* ধাপ ১৩: কমান্ডের পরে কী এল */
enum { SEP_END = 0, SEP_PIPE, SEP_AND, SEP_OR };
#define PIPE_MAX 16

struct command {
    char *argv[MAX_ARGS + 1];
    int argc;
    struct redirect redir[8];
    int nredir;
    int sep;                      /* ধাপ ১৩: SEP_* (; আর লাইনের শেষ = SEP_END) */
};

static char *dup_word(const char *s, size_t n)
{
    char *w = (char *)malloc(n + 1);
    if (w) {
        memcpy(w, s, n);
        w[n] = '\0';
    }
    return w;
}

/* $NAME, ${NAME}, $?, $$ — buf-এ জোড়া; *pp এগিয়ে যায় */
static void expand_var(const char **pp, char *word, size_t *wl)
{
    const char *p = *pp + 1;
    char name[128], val[32];
    const char *v = NULL;
    size_t n = 0;

    if (*p == '?') {
        snprintf(val, sizeof(val), "%d", last_status);
        v = val;
        p++;
    } else if (*p == '$') {
        snprintf(val, sizeof(val), "%d", getpid());
        v = val;
        p++;
    } else if (*p == '{') {
        p++;
        while (*p && *p != '}' && n + 1 < sizeof(name)) name[n++] = *p++;
        if (*p == '}') p++;
        name[n] = '\0';
        v = getenv(name);
    } else if (isalpha((unsigned char)*p) || *p == '_') {
        while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < sizeof(name)) name[n++] = *p++;
        name[n] = '\0';
        v = getenv(name);
    } else {
        if (*wl + 1 < WORD_MAX) word[(*wl)++] = '$';   /* একা $ */
        *pp = p;
        return;
    }
    if (v) {
        while (*v && *wl + 1 < WORD_MAX) word[(*wl)++] = *v++;
    }
    *pp = p;
}

/* এক কমান্ড (পরের ';' পর্যন্ত) পার্স করে; *pp পরের কমান্ডের শুরুতে। ত্রুটি হলে -1 */
static int parse_command(const char **pp, struct command *cmd)
{
    const char *p = *pp;
    char word[WORD_MAX];

    memset(cmd, 0, sizeof(*cmd));
    for (;;) {
        size_t wl = 0;
        int quoted = 0, redir_fd = -1, redir_flags = 0;

        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == ';' || *p == '#') {
            if (*p == ';') p++;
            else if (*p == '#') p += strlen(p);
            break;
        }
        /* ধাপ ১৩: | && || */
        if (*p == '|' || (p[0] == '&' && p[1] == '&')) {
            if (p[0] == '|' && p[1] == '|') { cmd->sep = SEP_OR; p += 2; }
            else if (p[0] == '|') { cmd->sep = SEP_PIPE; p++; }
            else { cmd->sep = SEP_AND; p += 2; }
            break;
        }

        /* রিডাইরেকশন চিহ্ন */
        if ((p[0] == '2' && p[1] == '>') || *p == '>' || *p == '<') {
            if (p[0] == '2') { redir_fd = 2; p++; }
            if (*p == '<') { redir_fd = 0; redir_flags = O_RDONLY; p++; }
            else {
                if (redir_fd < 0) redir_fd = 1;
                p++;
                if (*p == '>') { redir_flags = O_WRONLY | O_CREAT | O_APPEND; p++; }
                else redir_flags = O_WRONLY | O_CREAT | O_TRUNC;
            }
            if (*p == '&' && isdigit((unsigned char)p[1])) {        /* 2>&1 */
                if (cmd->nredir < 8) {
                    struct redirect *r = &cmd->redir[cmd->nredir++];
                    r->fd = redir_fd;
                    r->path = NULL;
                    r->dup_from = p[1] - '0';
                }
                p += 2;
                continue;
            }
            while (*p == ' ' || *p == '\t') p++;
        }

        while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '|' &&
               !(p[0] == '&' && p[1] == '&') &&
               !(*p == '>' || *p == '<') && wl + 1 < WORD_MAX) {
            if (*p == '\'') {
                quoted = 1;
                p++;
                while (*p && *p != '\'' && wl + 1 < WORD_MAX) word[wl++] = *p++;
                if (*p != '\'') { fprintf(stderr, "rozsh: missing '\n"); return -1; }
                p++;
            } else if (*p == '"') {
                quoted = 1;
                p++;
                while (*p && *p != '"' && wl + 1 < WORD_MAX) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        word[wl++] = p[1];
                        p += 2;
                    } else if (*p == '$') {
                        expand_var(&p, word, &wl);
                    } else {
                        word[wl++] = *p++;
                    }
                }
                if (*p != '"') { fprintf(stderr, "rozsh: missing \"\n"); return -1; }
                p++;
            } else if (*p == '\\' && p[1]) {
                word[wl++] = p[1];
                p += 2;
            } else if (*p == '$') {
                expand_var(&p, word, &wl);
            } else if (*p == '~' && wl == 0 && (p[1] == '/' || p[1] == ' ' || !p[1])) {
                const char *h = getenv("HOME");
                if (!h) h = "/home";
                while (*h && wl + 1 < WORD_MAX) word[wl++] = *h++;
                p++;
            } else {
                word[wl++] = *p++;
            }
        }
        if (wl == 0 && !quoted) {
            if (redir_fd >= 0) {
                fprintf(stderr, "rozsh: missing file name after redirection\n");
                return -1;
            }
            continue;
        }
        if (redir_fd >= 0) {
            if (cmd->nredir < 8) {
                struct redirect *r = &cmd->redir[cmd->nredir++];
                r->fd = redir_fd;
                r->flags = redir_flags;
                r->path = dup_word(word, wl);
            }
        } else if (cmd->argc < MAX_ARGS) {
            cmd->argv[cmd->argc++] = dup_word(word, wl);
        } else {
            fprintf(stderr, "rozsh: too many arguments\n");
            return -1;
        }
    }
    cmd->argv[cmd->argc] = NULL;
    *pp = p;
    return 0;
}

static void free_command(struct command *cmd)
{
    int i;
    for (i = 0; i < cmd->argc; i++) free(cmd->argv[i]);
    for (i = 0; i < cmd->nredir; i++) free(cmd->redir[i].path);
}

/* fd 0/1/2 বদলানো; save != NULL হলে আগেরগুলো রেখে দেয় (built-in-এর জন্য) */
static int apply_redirects(const struct command *cmd, int *save)
{
    int i;
    if (save) {
        save[0] = save[1] = save[2] = -1;
    }
    for (i = 0; i < cmd->nredir; i++) {
        const struct redirect *r = &cmd->redir[i];
        int fd;
        if (save && save[r->fd] < 0) {
            save[r->fd] = dup(r->fd);
        }
        if (r->path) {
            fd = open(r->path, r->flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "rozsh: %s: %s\n", r->path, strerror(errno));
                return -1;
            }
            if (fd != r->fd) {
                dup2(fd, r->fd);
                close(fd);
            }
        } else {
            dup2(r->dup_from, r->fd);
        }
    }
    return 0;
}

static void restore_fds(int *save)
{
    int i;
    fflush(stdout);
    fflush(stderr);
    for (i = 0; i < 3; i++) {
        if (save[i] >= 0) {
            dup2(save[i], i);
            close(save[i]);
        }
    }
}

/* ---------- built-in ---------- */

static int run_script(const char *path);

static int builtin_cd(int argc, char **argv)
{
    char cur[LINE_MAX_LEN];
    const char *dir = argc > 1 ? argv[1] : getenv("HOME");
    if (!dir) dir = "/";
    if (!strcmp(dir, "-")) {
        if (!prev_dir[0]) { fprintf(stderr, "cd: no previous directory\n"); return 1; }
        dir = prev_dir;
        printf("%s\n", dir);
    }
    if (!getcwd(cur, sizeof(cur))) cur[0] = '\0';
    if (chdir(dir) < 0) {
        fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    strcpy(prev_dir, cur);
    if (getcwd(cur, sizeof(cur))) setenv("PWD", cur, 1);
    return 0;
}

static void help_text(void)
{
    printf("rozsh - the rozOS shell\n"
           "  built-in : cd [dir|-], pwd, echo [-n], export [N=V], unset N, env, history,\n"
           "             source <file> (or . file), help, exit [code]\n"
           "  redirect : cmd < in, cmd > out, cmd >> out, cmd 2> err, cmd 2>&1\n"
           "  separate : cmd1 ; cmd2   pipe: cmd1 | cmd2   if: cmd1 && cmd2, cmd1 || cmd2\n"
           "  variables: $NAME ${NAME} $? $$   quotes: '' \"\" \\\n"
           "  keys     : arrows, Home/End, Delete, Up/Down = history, Ctrl+U/W/L/C, Ctrl+D = exit\n"
           "  programs : ls /bin   (edit = text editor, kmon = kernel monitor, systest = tests)\n"
           "  time cmd : how long a command took\n");
}

/* ধাপ ৮: পূর্ণ সাহায্য /bin/rozhelp-এ (লিংক বেছে Enter); না থাকলে ছোট তালিকা */
static int help(int argc, char **argv)
{
    struct stat st;
    if (stat("/bin/rozhelp", &st) == 0) {             /* কনসোল না হলে rozhelp সাধারণ লেখা ছাপে */
        char *args[3];
        int status;
        pid_t pid;
        args[0] = "rozhelp";
        args[1] = argc > 1 ? argv[1] : NULL;
        args[2] = NULL;
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            execv("/bin/rozhelp", args);
            _exit(127);
        }
        if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) != 127) {
            return WEXITSTATUS(status);
        }
    }
    help_text();
    return 0;
}

/* ফেরত: -1 = built-in নয়; নইলে status */
static int run_builtin(int argc, char **argv)
{
    const char *c = argv[0];
    int i;

    if (!strcmp(c, "cd")) return builtin_cd(argc, argv);
    if (!strcmp(c, "set")) {
        /* ধাপ ১১: set -e / set +e (make-এর রেসিপির জন্য) */
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-e")) errexit = 1;
            else if (!strcmp(argv[i], "+e")) errexit = 0;
            else { fprintf(stderr, "set: only -e and +e are supported\n"); return 2; }
        }
        return 0;
    }
    if (!strcmp(c, "true") || !strcmp(c, ":")) return 0;      /* ধাপ ১৩ */
    if (!strcmp(c, "exec")) {                                   /* ধাপ ১৪: shell-এর জায়গায় প্রোগ্রাম */
        if (argc < 2) return 0;
        fflush(stdout);
        execvp(argv[1], argv + 1);
        fprintf(stderr, "rozsh: exec: %s: %s\n", argv[1], strerror(errno));
        return 127;
    }
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "pwd")) {
        char buf[LINE_MAX_LEN];
        if (!getcwd(buf, sizeof(buf))) { perror("pwd"); return 1; }
        printf("%s\n", buf);
        return 0;
    }
    if (!strcmp(c, "exit")) {
        fflush(stdout);
        exit(argc > 1 ? atoi(argv[1]) : last_status);
    }
    if (!strcmp(c, "echo")) {
        int nl = 1, start = 1;
        if (argc > 1 && !strcmp(argv[1], "-n")) { nl = 0; start = 2; }
        for (i = start; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        if (nl) printf("\n");
        return 0;
    }
    if (!strcmp(c, "history")) {
        for (i = 0; i < hist_count; i++) printf("%4d  %s\n", i + 1, history[i]);
        return 0;
    }
    if (!strcmp(c, "export")) {
        extern char **environ;
        if (argc == 1) {
            for (i = 0; environ && environ[i]; i++) printf("export %s\n", environ[i]);
            return 0;
        }
        for (i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                setenv(argv[i], eq + 1, 1);
                *eq = '=';
            } else if (!getenv(argv[i])) {
                setenv(argv[i], "", 1);
            }
        }
        return 0;
    }
    if (!strcmp(c, "unset")) {
        for (i = 1; i < argc; i++) unsetenv(argv[i]);
        return 0;
    }
    if (!strcmp(c, "env")) {
        extern char **environ;
        for (i = 0; environ && environ[i]; i++) printf("%s\n", environ[i]);
        return 0;
    }
    if (!strcmp(c, "help")) {
        return help(argc, argv);
    }
    if (!strcmp(c, "source") || !strcmp(c, ".")) {
        if (argc < 2) { fprintf(stderr, "%s: file name needed\n", c); return 1; }
        return run_script(argv[1]);
    }
    return -1;
}

/* ---------- ধাপ ১৪: foreground কাজ ---------- */

static int job_control;           /* interactive shell টার্মিনালের মালিক */

/* সন্তানে: নিজের গ্রুপ, Ctrl+C আবার ডিফল্ট (shell-এর SIG_IGN উত্তরাধিকার নয়) */
static void child_job_setup(pid_t pgid)
{
    if (job_control) setpgid(0, pgid);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

static void job_foreground(pid_t pgid)
{
    if (job_control) {
        setpgid(pgid, pgid);
        tcsetpgrp(0, pgid);
    }
}

static void job_done(void)
{
    if (job_control) tcsetpgrp(0, getpgrp());
}

/* ---------- চালানো ---------- */

static int run_external(struct command *cmd)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "rozsh: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        child_job_setup(0);
        if (apply_redirects(cmd, NULL) < 0) _exit(1);
        execvp(cmd->argv[0], cmd->argv);
        if (errno == ENOENT) fprintf(stderr, "rozsh: %s: command not found\n", cmd->argv[0]);
        else fprintf(stderr, "rozsh: %s: %s\n", cmd->argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    job_foreground(pid);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { job_done(); return 1; }
    }
    job_done();
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
        fprintf(stderr, "^C\n");                /* ধাপ ১৪: Ctrl+C দিয়ে থামানো */
        return 128 + SIGINT;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "%s%s (signal %d)\n", cmd->argv[0],
                sig == 11 ? ": segmentation fault" : sig == 4 ? ": illegal instruction" :
                sig == 8 ? ": arithmetic error" : sig == 9 ? ": killed" : ": terminated", sig);
        return 128 + sig;
    }
    return WEXITSTATUS(status);
}

/* ধাপ ১৩: cmd[0] | cmd[1] | ... — প্রতিটা আলাদা প্রসেস (built-in হলেও, সন্তান প্রসেসে),
 * মাঝে pipe; ফল = শেষ কমান্ডের status (sh-এর মতো) */
static int run_pipeline(struct command *cmds, int n)
{
    pid_t pids[PIPE_MAX];
    int i, in_fd = -1, status = 0, result = 0;

    fflush(stdout);
    fflush(stderr);
    for (i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        if (i < n - 1 && pipe(fds) < 0) {
            fprintf(stderr, "rozsh: pipe: %s\n", strerror(errno));
            if (in_fd >= 0) close(in_fd);
            n = i;
            result = 1;
            break;
        }
        pids[i] = fork();
        if (pids[i] < 0) {
            fprintf(stderr, "rozsh: fork: %s\n", strerror(errno));
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            if (in_fd >= 0) close(in_fd);
            n = i;
            result = 1;
            break;
        }
        if (pids[i] == 0) {
            int r;
            child_job_setup(i == 0 ? 0 : pids[0]);
            if (in_fd >= 0) { dup2(in_fd, 0); close(in_fd); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); close(fds[0]); }
            if (apply_redirects(&cmds[i], NULL) < 0) _exit(1);
            if (cmds[i].argc == 0) _exit(0);
            r = run_builtin(cmds[i].argc, cmds[i].argv);
            if (r >= 0) {
                fflush(stdout);
                fflush(stderr);
                _exit(r);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            if (errno == ENOENT) fprintf(stderr, "rozsh: %s: command not found\n", cmds[i].argv[0]);
            else fprintf(stderr, "rozsh: %s: %s\n", cmds[i].argv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        if (job_control) setpgid(pids[i], pids[0]);
        if (i == 0) job_foreground(pids[0]);
        if (in_fd >= 0) close(in_fd);
        if (fds[1] >= 0) close(fds[1]);
        in_fd = fds[0];
    }
    for (i = 0; i < n; i++) {
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno != EINTR) break;
        }
        if (i == n - 1 && !result) {
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == SIGINT) fprintf(stderr, "^C\n");
                else if (sig != 13) fprintf(stderr, "%s: terminated (signal %d)\n", cmds[i].argv[0] ? cmds[i].argv[0] : "?", sig);
                result = 128 + sig;
            } else {
                result = WEXITSTATUS(status);
            }
        }
    }
    job_done();
    return result;
}

static void run_line(const char *line)
{
    const char *p = line;
    int skip = 0;                 /* ধাপ ১৩: && / || — পরের অংশ বাদ */
    while (*p) {
        struct command cmd;
        if (parse_command(&p, &cmd) < 0) {
            last_status = 2;
            return;
        }
        /* ধাপ ১৩: পাইপ — পুরো a | b | c একসাথে তুলে চালানো */
        if (cmd.sep == SEP_PIPE) {
            struct command cmds[PIPE_MAX];
            int n = 0, i, bad = 0;
            cmds[n++] = cmd;
            while (cmds[n - 1].sep == SEP_PIPE) {
                if (n == PIPE_MAX) {
                    fprintf(stderr, "rozsh: too many commands in a pipe (max %d)\n", PIPE_MAX);
                    bad = 1;
                    break;
                }
                if (!*p || parse_command(&p, &cmds[n]) < 0) {
                    if (!*p) fprintf(stderr, "rozsh: missing command after |\n");
                    bad = 1;
                    break;
                }
                if (cmds[n].argc == 0 && cmds[n].nredir == 0) {
                    fprintf(stderr, "rozsh: missing command after |\n");
                    free_command(&cmds[n]);
                    bad = 1;
                    break;
                }
                n++;
            }
            if (bad) {
                for (i = 0; i < n; i++) free_command(&cmds[i]);
                last_status = 2;
                return;
            }
            if (!skip) last_status = run_pipeline(cmds, n);
            cmd.sep = cmds[n - 1].sep;
            for (i = 0; i < n; i++) free_command(&cmds[i]);
            skip = (cmd.sep == SEP_AND && last_status != 0) || (cmd.sep == SEP_OR && last_status == 0);
            if (errexit && last_status != 0 && cmd.sep == SEP_END) return;
            continue;
        }
        if (skip) {
            free_command(&cmd);
            skip = (cmd.sep == SEP_AND && last_status != 0) || (cmd.sep == SEP_OR && last_status == 0);
            continue;
        }
        if (cmd.argc > 1 && !strcmp(cmd.argv[0], "time")) {
            /* time cmd args...: কমান্ড চালিয়ে সময় দেখানো */
            long t0 = rozos_now_ms(), dt;
            int i;
            free(cmd.argv[0]);
            for (i = 0; i < cmd.argc; i++) cmd.argv[i] = cmd.argv[i + 1];
            cmd.argc--;
            {
                int save[3], r;
                if (apply_redirects(&cmd, save) == 0) {
                    r = run_builtin(cmd.argc, cmd.argv);
                    restore_fds(save);
                    if (r < 0) r = run_external(&cmd);
                    last_status = r;
                } else {
                    restore_fds(save);
                    last_status = 1;
                }
            }
            dt = rozos_now_ms() - t0;
            fprintf(stderr, "real %ld.%03lds\n", dt / 1000, dt % 1000);
            free_command(&cmd);
            continue;
        }
        if (cmd.argc > 0) {
            int save[3], r;
            if (apply_redirects(&cmd, save) == 0) {
                r = run_builtin(cmd.argc, cmd.argv);
                restore_fds(save);
                if (r < 0) {
                    r = run_external(&cmd);
                }
                last_status = r;
            } else {
                restore_fds(save);
                last_status = 1;
            }
        } else if (cmd.nredir) {
            /* শুধু "> file": ফাইল তৈরি/খালি */
            int save[3];
            last_status = apply_redirects(&cmd, save) < 0;
            restore_fds(save);
        }
        free_command(&cmd);
        skip = (cmd.sep == SEP_AND && last_status != 0) || (cmd.sep == SEP_OR && last_status == 0);
        /* set -e: && / ||-এর বাঁ দিকের ব্যর্থতায় থামে না (sh-এর মতো) */
        if (errexit && last_status != 0 && cmd.sep != SEP_AND && cmd.sep != SEP_OR) return;
    }
}

static int run_script(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[LINE_MAX_LEN];
    if (!f) {
        fprintf(stderr, "rozsh: %s: %s\n", path, strerror(errno));
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        run_line(line);
        if (errexit && last_status != 0) break;
    }
    fclose(f);
    return last_status;
}

static void make_prompt(char *buf, size_t size)
{
    char cwd[LINE_MAX_LEN];
    const char *home = getenv("HOME"), *shown = cwd;
    char tmp[LINE_MAX_LEN];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    if (home && *home && strcmp(home, "/") && !strncmp(cwd, home, strlen(home)) &&
        (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0')) {
        snprintf(tmp, sizeof(tmp), "~%s", cwd + strlen(home));
        shown = tmp;
    }
    snprintf(buf, size, "\x1b[92mroot@%s\x1b[0m:\x1b[94m%s\x1b[0m%s ", hostname, shown,
             last_status ? "\x1b[91m$\x1b[0m" : "$");
}

int main(int argc, char **argv)
{
    char line[LINE_MAX_LEN], prompt[LINE_MAX_LEN + 64];
    struct utsname u;
    int login = argc > 1 && !strcmp(argv[1], "-l");

    if (argc > 2 && !strcmp(argv[1], "-c")) {
        /* ধাপ ১১: rozsh -c "command; command" — system() আর make এভাবে চালায় */
        interactive = 0;
        if (!getenv("PATH")) setenv("PATH", "/bin:/usr/local/bin:/home/bin", 1);
        run_line(argv[2]);
        return last_status;
    }
    if (argc > 1 && !login) {
        interactive = 0;
        return run_script(argv[1]);
    }
    interactive = isatty(0);
    if (interactive) {
        /* ধাপ ১৪: নিজের গ্রুপ, টার্মিনালের foreground; Ctrl+C-তে shell মরে না
         * (বাইটটা read_line পায় আর লাইন বাতিল করে) */
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        setpgid(0, 0);
        job_control = tcsetpgrp(0, getpgrp()) == 0;
    }
    if (uname(&u) == 0 && u.nodename[0]) {
        strncpy(hostname, u.nodename, sizeof(hostname) - 1);
    }
    if (!getenv("PATH")) setenv("PATH", "/bin:/usr/local/bin:/home/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/home", 1);
    if (interactive) {
        history_load();
        printf("\nrozsh on %s %s - type 'help' for commands, 'ls /bin' for programs\n",
               u.sysname, u.release);
    }
    if (login) {
        struct stat st;
        char rc[LINE_MAX_LEN];
        snprintf(rc, sizeof(rc), "%s/.rozshrc", getenv("HOME"));
        if (stat(rc, &st) == 0) run_script(rc);
    }
    for (;;) {
        int n;
        make_prompt(prompt, sizeof(prompt));
        n = read_line(prompt, line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;
        if (interactive) history_add(line, 1);
        run_line(line);
    }
    return last_status;
}
