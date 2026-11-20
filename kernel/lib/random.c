/* rozOS — ChaCha20-ভিত্তিক CSPRNG (rozos/random.h দেখুন) */

#include <stdint.h>
#include <stddef.h>
#include <rozos/random.h>
#include <rozos/string.h>
#include <rozos/bootinfo.h>
#include <arch/x86_64/cpu.h>

static uint32_t key[8];
static uint32_t pool[8];          /* নতুন entropy জমে, পরের random_get-এ key-তে মেশে */
static unsigned pool_pos;
static uint64_t nonce;
static int have_rdrand, have_rdseed;

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); \
    c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  \
    c += d; b ^= c; b = ROTL(b, 7)

static void chacha_block(const uint32_t k[8], uint64_t counter, uint64_t n, uint32_t out[16])
{
    uint32_t x[16], s[16];
    int i;
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (i = 0; i < 8; i++) s[4 + i] = k[i];
    s[12] = (uint32_t)counter;
    s[13] = (uint32_t)(counter >> 32);
    s[14] = (uint32_t)n;
    s[15] = (uint32_t)(n >> 32);
    memcpy(x, s, sizeof(x));
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) out[i] = x[i] + s[i];
}

static int rdrand64(uint64_t *v)
{
    unsigned char ok;
    int tries;
    for (tries = 0; tries < 10; tries++) {
#ifdef __TINYC__
        /* ধাপ ১১: TCC rdrand চেনে না — REX.W 0F C7 /6, ফল rax-এ */
        __asm__ __volatile__(".byte 0x48, 0x0f, 0xc7, 0xf0; setc %1" : "=a"(*v), "=qm"(ok));
#else
        __asm__ __volatile__("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
#endif
        if (ok) return 1;
    }
    return 0;
}

static int rdseed64(uint64_t *v)
{
    unsigned char ok;
    int tries;
    for (tries = 0; tries < 10; tries++) {
#ifdef __TINYC__
        /* REX.W 0F C7 /7 = rdseed rax */
        __asm__ __volatile__(".byte 0x48, 0x0f, 0xc7, 0xf8; setc %1" : "=a"(*v), "=qm"(ok));
#else
        __asm__ __volatile__("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
#endif
        if (ok) return 1;
    }
    return 0;
}

void random_add_entropy(uint64_t v)
{
    unsigned i = pool_pos++ & 7, j = (i + 3) & 7;
    pool[i] ^= (uint32_t)v;
    pool[j] = ROTL(pool[j] + (uint32_t)(v >> 32) + pool[i], 7) ^ pool[(i + 5) & 7];
}

int random_has_rdrand(void)
{
    return have_rdrand;
}

void random_init(void)
{
    uint32_t a, b, c, d;
    uint64_t v;
    int i;

    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_rdrand = (c >> 30) & 1;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    have_rdseed = (b >> 18) & 1;

    random_add_entropy(cpu_rdtsc());
    random_add_entropy((uint64_t)bootinfo()->boot_unix_time);
    for (i = 0; i < 64; i++) {
        /* TSC jitter: কয়েকটা নির্দেশের মাঝের ছোট অনিশ্চয়তা */
        uint64_t t = cpu_rdtsc();
        __asm__ __volatile__("pause");
        random_add_entropy(cpu_rdtsc() - t + ((uint64_t)i << 32));
    }
    for (i = 0; i < 8; i++) {
        if ((have_rdseed && rdseed64(&v)) || (have_rdrand && rdrand64(&v))) {
            random_add_entropy(v);
        }
    }
}

void random_get(void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t block[16];
    uint64_t counter = 1, flags;
    int i;

    flags = cpu_irq_save();
    /* জমা entropy key-তে মেশানো */
    for (i = 0; i < 8; i++) {
        key[i] ^= pool[i];
        pool[i] = 0;
    }
    key[0] ^= (uint32_t)cpu_rdtsc();
    if (have_rdrand) {
        uint64_t v;
        if (rdrand64(&v)) { key[1] ^= (uint32_t)v; key[2] ^= (uint32_t)(v >> 32); }
    }
    nonce++;
    /* block 0 থেকে নতুন key (fast key erasure), বাকি ব্লক আউটপুট */
    chacha_block(key, 0, nonce, block);
    {
        uint32_t newkey[8];
        memcpy(newkey, block, sizeof(newkey));
        while (len > 0) {
            size_t n = len < 64 ? len : 64;
            chacha_block(key, counter++, nonce, block);
            memcpy(out, block, n);
            out += n;
            len -= n;
        }
        memcpy(key, newkey, sizeof(key));
    }
    memset(block, 0, sizeof(block));
    cpu_irq_restore(flags);
}
