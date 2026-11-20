/* rozOS — http-get: http বা https দিয়ে একটা পাতা আনা (ধাপ ৯; ধাপ ১০-এ librozhttp + TLS)
 *
 *   http-get [-h] [-q] [-v] [-n] [-o file] http[s]://host[:port]/path
 *   -h        শুধু হেডার দেখানো (stdout-এ)     -o file   শরীরটা ফাইলে লেখা
 *   -q        চুপচাপ (হেডার/সারাংশ নয়)         -v        সংযোগ/redirect-এর ধাপ (ডিফল্টই চালু; -q-এর সাথে দিলে আবার চালু)
 *   -n        redirect (301/302...) অনুসরণ না করা */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <rozos.h>
#include "rozhttp.h"

int main(int argc, char **argv)
{
    const char *outfile = NULL;
    struct rozhttp_opts o;
    struct rozhttp_result r;
    int out = 1, opt, headers_only = 0, quiet = 0, rc;
    long t0;

    memset(&o, 0, sizeof(o));
    o.header_fd = 2;
    o.max_redirects = 5;
    while ((opt = getopt(argc, argv, "ho:qvn")) != -1) {
        if (opt == 'h') headers_only = 1;
        else if (opt == 'o') outfile = optarg;
        else if (opt == 'q') quiet = 1;
        else if (opt == 'v') o.verbose = 1;
        else if (opt == 'n') o.no_follow = 1;
        else return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: http-get [-h] [-q] [-v] [-n] [-o file] http[s]://host[:port]/path\n");
        return 2;
    }
    if (!strncmp(argv[optind], "https://", 8) && !rozhttp_tls_available()) {
        fprintf(stderr, "http-get: this build has no TLS (BearSSL) — https is unavailable\n");
        return 2;
    }
    if (headers_only) {
        o.header_fd = 1;
        o.no_body = 1;
    } else if (quiet) {
        o.header_fd = -1;
    }
    /* ধাপ ৯-এর মতো "connecting to ..." সবসময় দেখানো (-q ছাড়া); -v আগে থেকেই চালু রাখে */
    if (!quiet) o.verbose = 1;
    if (outfile) {
        out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { fprintf(stderr, "http-get: %s: %s\n", outfile, strerror(errno)); return 1; }
        o.progress = !quiet;
    }
    t0 = rozos_now_ms();
    rc = rozhttp_get(argv[optind], out, &o, &r);
    if (outfile) close(out);
    if (rc < 0) {
        fprintf(stderr, "http-get: %s\n", r.error);
        return 1;
    }
    if (!quiet) {
        long ms = rozos_now_ms() - t0;
        if (headers_only) {
            fprintf(stderr, "HTTP %d%s, headers only, %ld ms\n", r.status, r.tls ? " (TLS)" : "", ms);
        } else {
            fprintf(stderr, "\n%ld bytes in %ld ms%s%s\n", r.bytes, ms, r.tls ? ", TLS" : "",
                    r.redirects ? ", after redirect" : "");
        }
        if (r.redirects) fprintf(stderr, "final URL: %s\n", r.final_url);
    }
    return 0;
}
