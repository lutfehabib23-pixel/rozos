/* rozOS — librozlock: ফোল্ডার লক (encryption) আর লুকানো (ধাপ ১৯ছ)
 *
 * লক: পুরো ফোল্ডার (ভেতরের ফাইলের নাম আর কাঠামোসহ) একটা encrypted ফাইলে যায় —
 * ফোল্ডারের পাশে "NAME.rozlock"; মূল ফাইলগুলো শূন্য দিয়ে ঢেকে মোছা হয়। লক-ফাইলটা
 * immutable (chattr +i) — মোছা, নাম বদল, লেখা যায় না। আনলক: secret ঠিক হলে ফোল্ডার
 * আগের মতো ফেরে আর লক-ফাইল মোছে।
 *
 *   key  = PBKDF2-HMAC-SHA256(secret, 16 বাইট এলোমেলো salt, 100000 বার)
 *   ডেটা = ChaCha20-Poly1305, ৬৪ KiB-এর টুকরো; প্রতিটার নিজের nonce আর tag — ভুল secret,
 *          বদলানো বা কাটা ফাইল সব ধরা পড়ে। secret ভুলে গেলে ডেটা ফেরানোর কোনো উপায় নেই।
 *
 * লুকানো: ফোল্ডারের ".hidden" ফাইলে নামের তালিকা (প্রতি লাইনে একটা — GNOME/macOS-এর
 * নিয়ম)। নাম বদলায় না; Files, ডেস্কটপ আর ls এগুলো দেখায় না (Files-এ View > Hidden
 * files, ls -a)। এটা সুরক্ষা নয় — শুধু চোখের আড়াল।
 *
 *   #include <rozlock.h>
 *   int r = rozlock_lock("/home/secret", "my words", NULL, NULL, out, sizeof(out));
 *   if (r < 0) printf("%s\n", rozlock_strerror(r));
 *   r = rozlock_unlock("/home/secret.rozlock", "my words", NULL, NULL, out, sizeof(out));
 *
 * লিংক: -lrozlock -lbearssl */
#ifndef ROZLOCK_H
#define ROZLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROZLOCK_EXT ".rozlock"

/* ফেরত মান: 0 = ঠিক, < 0 = ভুল — -errno অথবা নিচের কোনোটা */
#define ROZLOCK_EWRONG    (-1000)   /* secret ভুল */
#define ROZLOCK_EFORMAT   (-1001)   /* লক-ফাইল নয়, বা নষ্ট/বদলানো */
#define ROZLOCK_ENOCRYPTO (-1002)   /* BearSSL ছাড়া বানানো */
#define ROZLOCK_ESYSTEM   (-1003)   /* সিস্টেমের ফোল্ডার (/, /bin, /home ...) লক করা যায় না */
#define ROZLOCK_EPARTIAL  (-1004)   /* লক হয়েছে, কিন্তু কিছু মূল ফাইল মোছা যায়নি */

/* অগ্রগতি: done/total বাইট, name = এখন যে ফাইল (NULL হতে পারে) */
typedef void (*rozlock_progress_fn)(void *ctx, uint64_t done, uint64_t total, const char *name);

/* dir লক করা; সফল হলে out-এ লক-ফাইলের পথ */
int rozlock_lock(const char *dir, const char *secret, rozlock_progress_fn cb, void *ctx,
                 char *out, size_t outlen);
/* লক-ফাইল (বা "NAME" দিলে "NAME.rozlock") খোলা; সফল হলে out-এ ফেরা ফোল্ডারের পথ।
 * একই নামে কিছু থাকলে "NAME 2", "NAME 3" ... */
int rozlock_unlock(const char *lockfile, const char *secret, rozlock_progress_fn cb, void *ctx,
                   char *out, size_t outlen);
/* secret ঠিক কিনা — শুধু প্রথম টুকরো খুলে দেখে (কিছু বদলায় না) */
int rozlock_check(const char *lockfile, const char *secret);
int rozlock_is_lockfile(const char *path);          /* 1 = rozlock ফাইল */
/* "a/b/Photos.rozlock" → "Photos" */
void rozlock_display_name(const char *path, char *out, size_t outlen);
/* immutable (মোছা-রোধী) বসানো/তোলা: 0 = ঠিক; FAT-এ -EOPNOTSUPP */
int rozlock_protect(const char *path, int on);
int rozlock_is_protected(const char *path);         /* 1 = immutable */
const char *rozlock_strerror(int err);

/* ---------- লুকানো (.hidden) ---------- */
struct rozhide {
    char *data;
    char **names;
    int n;
};
int  rozhide_load(struct rozhide *h, const char *dir);    /* না থাকলে খালি তালিকা, 0 */
int  rozhide_has(const struct rozhide *h, const char *name);
void rozhide_free(struct rozhide *h);
int  rozhide_is_hidden(const char *path);                 /* পুরো পথ */
int  rozhide_set(const char *path, int hidden);           /* 0 = ঠিক */

#ifdef __cplusplus
}
#endif

#endif
