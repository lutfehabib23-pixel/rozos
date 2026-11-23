/* rozOS — rozlock: ফোল্ডার লক/আনলক আর লুকানো, টার্মিনাল থেকে (ধাপ ১৯ছ)
 *
 *   rozlock lock DIR          ফোল্ডার encrypt করে DIR.rozlock (secret দুবার চায়)
 *   rozlock unlock PATH       PATH = NAME.rozlock বা NAME — secret ঠিক হলে ফোল্ডার ফেরে
 *   rozlock check PATH        secret ঠিক কিনা দেখা (কিছু বদলায় না)
 *   rozlock status PATH...    লক করা কিনা, লুকানো কিনা, মোছা-রোধী কিনা
 *   rozlock hide PATH...      .hidden তালিকায় তোলা (নাম বদলায় না)
 *   rozlock unhide PATH...
 *
 * secret: টার্মিনালে লেখা দেখায় না। stdin টার্মিনাল না হলে প্রথম লাইনটাই secret
 * (যেমন: echo "my words" | rozlock unlock Photos)। ভুলে গেলে ডেটা ফেরানো যায় না।
 * গ্রাফিক্সে একই কাজ: Files বা ডেস্কটপে ডান-ক্লিক > Lock... / Unlock... (lockdlg)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rozlock.h>

static int quiet;

static void progress(void *ctx, uint64_t done, uint64_t total, const char *name)
{
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 0;
    (void)ctx; (void)name;
    if (quiet || !isatty(2) || pct == last) return;
    last = pct > 100 ? 100 : pct;
    fprintf(stderr, "\r  %3d%%", last);
}

static int get_secret(char *buf, size_t n, const char *prompt)
{
    if (isatty(0)) {
        char *p = getpass(prompt);
        if (!p) return -1;
        snprintf(buf, n, "%s", p);
        memset(p, 0, strlen(p));
    } else {
        if (!fgets(buf, (int)n, stdin)) return -1;
        buf[strcspn(buf, "\r\n")] = '\0';
    }
    return buf[0] ? 0 : -1;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: rozlock lock DIR\n"
            "       rozlock unlock NAME[.rozlock]\n"
            "       rozlock check NAME[.rozlock]\n"
            "       rozlock status PATH...\n"
            "       rozlock hide|unhide PATH...\n"
            "options: -q (no progress)\n"
            "see: help rozlock\n");
    return 2;
}

int main(int argc, char **argv)
{
    char s1[256], s2[256], out[1100];
    const char *cmd;
    int r, i, rc = 0;
    if (argc > 1 && !strcmp(argv[1], "-q")) { quiet = 1; argv++; argc--; }
    if (argc < 3) return usage();
    cmd = argv[1];
    if (!strcmp(cmd, "lock")) {
        if (argc != 3) return usage();
        if (isatty(0))
            fprintf(stderr, "Locking %s\nIf you forget the secret, the files can NEVER be recovered.\n", argv[2]);
        if (get_secret(s1, sizeof(s1), "Secret: ") < 0) { fprintf(stderr, "rozlock: no secret\n"); return 1; }
        if (isatty(0)) {
            if (get_secret(s2, sizeof(s2), "Again:  ") < 0 || strcmp(s1, s2)) {
                fprintf(stderr, "rozlock: the two secrets are not the same\n");
                return 1;
            }
            if (strlen(s1) < 4) { fprintf(stderr, "rozlock: use at least 4 characters\n"); return 1; }
        }
        r = rozlock_lock(argv[2], s1, progress, NULL, out, sizeof(out));
        memset(s1, 0, sizeof(s1));
        memset(s2, 0, sizeof(s2));
        if (!quiet && isatty(2)) fprintf(stderr, "\r");
        if (r < 0 && r != ROZLOCK_EPARTIAL) { fprintf(stderr, "rozlock: %s: %s\n", argv[2], rozlock_strerror(r)); return 1; }
        printf("locked: %s\n", out);
        if (r < 0) fprintf(stderr, "rozlock: %s\n", rozlock_strerror(r));
        return r < 0;
    }
    if (!strcmp(cmd, "unlock") || !strcmp(cmd, "check")) {
        if (argc != 3) return usage();
        if (get_secret(s1, sizeof(s1), "Secret: ") < 0) { fprintf(stderr, "rozlock: no secret\n"); return 1; }
        if (cmd[0] == 'c') r = rozlock_check(argv[2], s1);
        else r = rozlock_unlock(argv[2], s1, progress, NULL, out, sizeof(out));
        memset(s1, 0, sizeof(s1));
        if (!quiet && isatty(2) && cmd[0] == 'u') fprintf(stderr, "\r");
        if (r < 0) { fprintf(stderr, "rozlock: %s: %s\n", argv[2], rozlock_strerror(r)); return 1; }
        if (cmd[0] == 'c') printf("secret OK\n");
        else printf("unlocked: %s\n", out);
        return 0;
    }
    if (!strcmp(cmd, "status")) {
        for (i = 2; i < argc; i++) {
            int lk = rozlock_is_lockfile(argv[i]);
            if (access(argv[i], F_OK) != 0) { fprintf(stderr, "rozlock: %s: not found\n", argv[i]); rc = 1; continue; }
            printf("%s: %s%s%s\n", argv[i], lk ? "locked folder" : "not locked",
                   rozhide_is_hidden(argv[i]) ? ", hidden" : "",
                   rozlock_is_protected(argv[i]) ? ", cannot be deleted" : "");
        }
        return rc;
    }
    if (!strcmp(cmd, "hide") || !strcmp(cmd, "unhide")) {
        for (i = 2; i < argc; i++) {
            if (access(argv[i], F_OK) != 0) { fprintf(stderr, "rozlock: %s: not found\n", argv[i]); rc = 1; continue; }
            if ((r = rozhide_set(argv[i], cmd[0] == 'h')) < 0) {
                fprintf(stderr, "rozlock: %s: %s\n", argv[i], rozlock_strerror(r));
                rc = 1;
            }
        }
        return rc;
    }
    return usage();
}
