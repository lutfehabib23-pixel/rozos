/* rozOS — rozhttp: ছোট http/https ক্লায়েন্ট লাইব্রেরি (ধাপ ১০)
 *
 * http-get আর rozpkg ব্যবহার করে; /usr/lib/librozhttp.a + /usr/include/rozhttp.h হিসেবে
 * অন্য প্রোগ্রামও পারে (tcc prog.c -lrozhttp -lbearssl)।
 *   - http:// ও https:// (BearSSL, TLS 1.2; সার্টিফিকেট /etc/ssl/certs.pem দিয়ে যাচাই)
 *   - HTTP/1.1 GET, redirect মানা (301/302/303/307/308), chunked ও Content-Length */
#ifndef ROZHTTP_H
#define ROZHTTP_H

#include <stddef.h>

/* ধাপ ১৭গ: C++ থেকেও (g++) */
#ifdef __cplusplus
extern "C" {
#endif

#define ROZHTTP_URL_MAX 2048

struct rozhttp_opts {
    int header_fd;                 /* হেডার কোথায় লিখবে (-1 = কোথাও না) */
    int no_body;                   /* 1 = শরীর পড়বে না (শুধু হেডার) */
    int max_redirects;             /* 0 হলে 5 */
    int no_follow;                 /* 1 = redirect অনুসরণ না করে সেই উত্তরটাই ফেরত */
    int progress;                  /* 1 = stderr-এ কত KiB এল দেখায় */
    int verbose;                   /* 1 = সংযোগের ধাপগুলো stderr-এ */
    /* প্রতিটি শরীরের টুকরো (ফাইলে লেখার পাশাপাশি) — যেমন SHA-256 হিসাবের জন্য */
    void (*on_data)(void *ctx, const void *data, size_t len);
    void *on_data_ctx;
};

struct rozhttp_result {
    int  status;                   /* শেষ উত্তরের HTTP status (200 ...) */
    long bytes;                    /* শরীরের কত বাইট এল */
    long content_length;           /* সার্ভার যা বলেছে (-1 = জানা নেই) */
    int  redirects;
    int  tls;                      /* শেষ সংযোগ https ছিল কিনা */
    char final_url[ROZHTTP_URL_MAX];
    char error[256];               /* ব্যর্থ হলে কারণ */
};

/* url আনা, শরীর out_fd-তে (-1 = শুধু on_data)। সফল (2xx) হলে 0, নইলে -1 (r->error) */
int rozhttp_get(const char *url, int out_fd, const struct rozhttp_opts *opts, struct rozhttp_result *r);

/* https আছে কিনা (BearSSL সহ বিল্ড), আর কয়টা বিশ্বস্ত CA লোড হলো (প্রথমবার ডাকলে লোড করে) */
int rozhttp_tls_available(void);
int rozhttp_trust_anchor_count(void);

/* SHA-256 (rozpkg-এর যাচাই) — BearSSL না থাকলে নিজস্ব ছোট বাস্তবায়ন */
struct rozhttp_sha256 { unsigned char opaque[128]; };
void rozhttp_sha256_init(struct rozhttp_sha256 *s);
void rozhttp_sha256_update(struct rozhttp_sha256 *s, const void *data, size_t len);
void rozhttp_sha256_hex(struct rozhttp_sha256 *s, char out[65]);

#ifdef __cplusplus
}
#endif
#endif
