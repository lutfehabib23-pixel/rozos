/* rozOS — cref: C ভাষা আর লাইব্রেরির সহায়িকা, টার্মিনালে (ধাপ ১৯ক)
 *
 *   cref printf        পাতা: syntax, ব্যাখ্যা, ফেরত মান, উদাহরণ
 *   cref -k copy       নাম/বর্ণনায় "copy" আছে এমন সব
 *   cref -l            হেডারের তালিকা;  cref -l string.h  সেই হেডারের সব
 *   cref -x printf     শুধু উদাহরণ (cref -x printf > p.c; tcc p.c -o p; ./p)
 *   cref -s printf     শুধু syntax
 * উইন্ডোতে দেখতে: crefview; Code আর edit-এ Ctrl+F1। */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <rozcref.h>

static int term_width(void)
{
    struct winsize ws;
    if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40) return ws.ws_col - 1;
    return 79;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: cref NAME          show the help page (function or topic)\n"
            "       cref -k WORD       search names and descriptions\n"
            "       cref -l [HEADER]   list headers, or everything in one (e.g. cref -l stdio.h)\n"
            "       cref -x NAME       print only the example program\n"
            "       cref -s NAME       print only the syntax\n"
            "The same pages: crefview (window), Ctrl+F1 in Code and edit. See: help cref\n");
}

static void list_line(const cref_entry *e)
{
    printf("  %-16s %-12s %s\n", e->name, strcmp(e->header, "language") ? e->header : "(language)", e->summary);
}

static int not_found(const char *name)
{
    const cref_entry *hits[12];
    int n = cref_search(name, hits, 12), i;
    fprintf(stderr, "cref: no page called '%s'\n", name);
    if (n > 0) {
        fprintf(stderr, "Maybe:\n");
        for (i = 0; i < n; i++) fprintf(stderr, "  %-16s %s\n", hits[i]->name, hits[i]->summary);
    } else {
        fprintf(stderr, "Try: cref -k %s   or   cref -l\n", name);
    }
    return 1;
}

int main(int argc, char **argv)
{
    const cref_entry *e;
    int i;
    if (cref_load(NULL) < 0) {
        fprintf(stderr, "cref: no reference files in %s\n", CREF_DIR);
        return 1;
    }
    if (argc < 2) {
        const char *h[64];
        int n = cref_headers(h, 64);
        printf("C reference: %d pages. Headers and topics:\n", cref_count());
        for (i = 0; i < n; i++) printf("  %s\n", h[i]);
        printf("\n");
        usage();
        return 0;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    if (!strcmp(argv[1], "-k")) {
        const cref_entry *hits[400];
        int n;
        if (argc < 3) { usage(); return 2; }
        n = cref_search(argv[2], hits, 400);
        if (n == 0) { fprintf(stderr, "cref: nothing found for '%s'\n", argv[2]); return 1; }
        for (i = 0; i < n; i++) list_line(hits[i]);
        return 0;
    }
    if (!strcmp(argv[1], "-l")) {
        int n = cref_count(), found = 0;
        if (argc < 3) {
            const char *h[64];
            int nh = cref_headers(h, 64);
            for (i = 0; i < nh; i++) {
                int k, c = 0;
                for (k = 0; k < n; k++) if (!strcmp(cref_at(k)->header, h[i])) c++;
                printf("  %-14s %3d pages\n", h[i], c);
            }
            return 0;
        }
        for (i = 0; i < n; i++) {
            e = cref_at(i);
            if (!strcmp(e->header, argv[2]) || (!strcmp(argv[2], "lang") && !strcmp(e->header, "language"))) {
                list_line(e);
                found++;
            }
        }
        if (!found) { fprintf(stderr, "cref: no header '%s' (cref -l lists them)\n", argv[2]); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "-x") || !strcmp(argv[1], "-s")) {
        if (argc < 3) { usage(); return 2; }
        e = cref_find(argv[2]);
        if (!e) return not_found(argv[2]);
        if (argv[1][1] == 'x') {
            if (!*e->example) { fprintf(stderr, "cref: '%s' has no example\n", e->name); return 1; }
            printf("%s\n", e->example);
        } else {
            printf("%s\n", *e->syntax ? e->syntax : e->name);
        }
        return 0;
    }
    if (argv[1][0] == '-') { usage(); return 2; }
    e = cref_find(argv[1]);
    if (!e) return not_found(argv[1]);
    {
        char *t = cref_format(e, term_width());
        if (t) fputs(t, stdout);
        free(t);
    }
    return 0;
}
