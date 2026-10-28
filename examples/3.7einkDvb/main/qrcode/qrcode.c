#include "qrcode.h"

#include <stdbool.h>
#include <string.h>

/* QR v1–v4, ECC M, byte mode。覆盖 http://x.x.x.x 短 URL。 */

static const int k_total_cw[] = { 0, 26, 44, 70, 100 };
static const int k_ec_cw[]    = { 0, 10, 16, 26, 36 };
static const int k_align[]    = { 0, 0, 18, 22, 26 };

static const uint16_t k_fmt_m[8] = {
    0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x45F9, 0x40CE, 0x4F97, 0x4AA0
};

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool s_gf_ok;

static void gf_init(void)
{
    uint8_t x = 1;
    if (s_gf_ok) {
        return;
    }
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = (uint8_t)i;
        x = (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x11D : 0));
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    s_gf_ok = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return gf_exp[gf_log[a] + gf_log[b]];
}

static void rs_encode(const uint8_t *data, int data_len, uint8_t *ec, int ec_len)
{
    uint8_t gen[32];
    uint8_t root = 1;

    memset(gen, 0, sizeof(gen));
    gen[ec_len - 1] = 1;
    for (int i = 0; i < ec_len; i++) {
        for (int j = 0; j < ec_len; j++) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < ec_len) {
                gen[j] ^= gen[j + 1];
            }
        }
        root = gf_mul(root, 2);
    }

    memset(ec, 0, (size_t)ec_len);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ ec[0];
        memmove(ec, ec + 1, (size_t)(ec_len - 1));
        ec[ec_len - 1] = 0;
        if (factor == 0) {
            continue;
        }
        for (int j = 0; j < ec_len; j++) {
            ec[j] ^= gf_mul(gen[j], factor);
        }
    }
}

static void put_bits(uint8_t *buf, int *bit, int nbits, unsigned value)
{
    for (int i = nbits - 1; i >= 0; i--) {
        int pos = *bit;
        if ((value >> i) & 1u) {
            buf[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
        }
        (*bit)++;
    }
}

static int version_for(int nbytes)
{
    const int payload_cw = nbytes + 2;
    if (payload_cw <= 16) return 1;
    if (payload_cw <= 28) return 2;
    if (payload_cw <= 44) return 3;
    if (payload_cw <= 64) return 4;
    return 0;
}

static int qr_size_of(int ver)
{
    return 21 + 4 * (ver - 1);
}

static void set_mod(uint8_t *m, int size, int x, int y, int v)
{
    if ((unsigned)x < (unsigned)size && (unsigned)y < (unsigned)size) {
        m[y * size + x] = (uint8_t)(v ? 1 : 0);
    }
}

static uint8_t get_mod(const uint8_t *m, int size, int x, int y)
{
    if ((unsigned)x >= (unsigned)size || (unsigned)y >= (unsigned)size) {
        return 0;
    }
    return m[y * size + x];
}

static void finder(uint8_t *m, uint8_t *fn, int size, int ox, int oy)
{
    for (int y = -1; y <= 7; y++) {
        for (int x = -1; x <= 7; x++) {
            int xx = ox + x;
            int yy = oy + y;
            if ((unsigned)xx >= (unsigned)size || (unsigned)yy >= (unsigned)size) {
                continue;
            }
            int on = (x >= 0 && x <= 6 && y >= 0 && y <= 6) &&
                     (x == 0 || x == 6 || y == 0 || y == 6 ||
                      (x >= 2 && x <= 4 && y >= 2 && y <= 4));
            set_mod(m, size, xx, yy, on);
            fn[yy * size + xx] = 1;
        }
    }
}

static void timing_and_dark(uint8_t *m, uint8_t *fn, int size)
{
    for (int i = 8; i < size - 8; i++) {
        set_mod(m, size, 6, i, (i & 1) == 0);
        set_mod(m, size, i, 6, (i & 1) == 0);
        fn[i * size + 6] = 1;
        fn[6 * size + i] = 1;
    }
    set_mod(m, size, 8, size - 8, 1);
    fn[(size - 8) * size + 8] = 1;
}

static void alignment(uint8_t *m, uint8_t *fn, int size, int ver)
{
    int p = k_align[ver];
    if (p == 0) {
        return;
    }
    for (int y = p - 2; y <= p + 2; y++) {
        for (int x = p - 2; x <= p + 2; x++) {
            int on = (x == p - 2 || x == p + 2 || y == p - 2 || y == p + 2 || (x == p && y == p));
            set_mod(m, size, x, y, on);
            fn[y * size + x] = 1;
        }
    }
}

static void reserve_format(uint8_t *fn, int size)
{
    for (int i = 0; i < 9; i++) {
        fn[8 * size + i] = 1;
        fn[i * size + 8] = 1;
    }
    for (int i = 0; i < 8; i++) {
        fn[8 * size + (size - 1 - i)] = 1;
        fn[(size - 1 - i) * size + 8] = 1;
    }
}

static int mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((x + y) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((x + y) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: return ((x * y) % 2 + (x * y) % 3) == 0;
    case 6: return ((((x * y) % 2) + ((x * y) % 3)) & 1) == 0;
    default: return ((((x + y) % 2) + ((x * y) % 3)) & 1) == 0;
    }
}

static void place_data(uint8_t *m, const uint8_t *fn, int size,
                       const uint8_t *bits, int nbits, int mask)
{
    int bit = 0;
    int dir = -1;
    int x = size - 1;
    int y = size - 1;
    while (x > 0) {
        if (x == 6) {
            x--;
        }
        for (int i = 0; i < 2; i++) {
            int xx = x - i;
            if (!fn[y * size + xx]) {
                int v = 0;
                if (bit < nbits) {
                    v = (bits[bit >> 3] & (0x80 >> (bit & 7))) ? 1 : 0;
                }
                if (mask_bit(mask, xx, y)) {
                    v ^= 1;
                }
                set_mod(m, size, xx, y, v);
                bit++;
            }
        }
        y += dir;
        if (y < 0 || y >= size) {
            dir = -dir;
            y += dir;
            x -= 2;
        }
    }
}

static void write_format(uint8_t *m, int size, int mask)
{
    uint16_t bits = k_fmt_m[mask];
    for (int i = 0; i < 6; i++) {
        set_mod(m, size, i, 8, (bits >> i) & 1);
    }
    set_mod(m, size, 7, 8, (bits >> 6) & 1);
    set_mod(m, size, 8, 8, (bits >> 7) & 1);
    set_mod(m, size, 8, 7, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) {
        set_mod(m, size, 8, 14 - i, (bits >> i) & 1);
    }
    for (int i = 0; i < 8; i++) {
        set_mod(m, size, size - 1 - i, 8, (bits >> i) & 1);
    }
    for (int i = 0; i < 7; i++) {
        set_mod(m, size, 8, size - 7 + i, (bits >> (8 + i)) & 1);
    }
}

static int penalty(const uint8_t *m, int size)
{
    int score = 0;
    for (int y = 0; y < size; y++) {
        int run = 1;
        for (int x = 1; x < size; x++) {
            if (get_mod(m, size, x, y) == get_mod(m, size, x - 1, y)) {
                run++;
            } else {
                if (run >= 5) {
                    score += run - 2;
                }
                run = 1;
            }
        }
        if (run >= 5) {
            score += run - 2;
        }
    }
    for (int x = 0; x < size; x++) {
        int run = 1;
        for (int y = 1; y < size; y++) {
            if (get_mod(m, size, x, y) == get_mod(m, size, x, y - 1)) {
                run++;
            } else {
                if (run >= 5) {
                    score += run - 2;
                }
                run = 1;
            }
        }
        if (run >= 5) {
            score += run - 2;
        }
    }
    return score;
}

int qrcode_encode(const char *text, uint8_t *modules, int modules_cap, int *out_size)
{
    uint8_t data[80];
    uint8_t ec[40];
    uint8_t bits[128];
    uint8_t fn[QR_MAX_SIZE * QR_MAX_SIZE];
    uint8_t trial[QR_MAX_SIZE * QR_MAX_SIZE];
    int n = text ? (int)strlen(text) : 0;
    int ver = version_for(n);
    int size;
    int data_cw;
    int ec_len;
    int bit = 0;
    int nbits;
    int best_mask = 0;
    int best_pen = 0x7fffffff;

    if (ver == 0 || modules == NULL || n > 64) {
        return 0;
    }
    size = qr_size_of(ver);
    if (size * size > modules_cap) {
        return 0;
    }

    gf_init();
    data_cw = k_total_cw[ver] - k_ec_cw[ver];
    ec_len = k_ec_cw[ver];

    memset(data, 0, sizeof(data));
    put_bits(data, &bit, 4, 0x4);
    put_bits(data, &bit, 8, (unsigned)n);
    for (int i = 0; i < n; i++) {
        put_bits(data, &bit, 8, (unsigned)(uint8_t)text[i]);
    }
    put_bits(data, &bit, 4, 0);
    while ((bit & 7) != 0) {
        put_bits(data, &bit, 1, 0);
    }
    {
        int pad = 0;
        while ((bit >> 3) < data_cw) {
            put_bits(data, &bit, 8, (pad++ & 1) ? 0x11 : 0xEC);
        }
    }

    rs_encode(data, data_cw, ec, ec_len);
    memset(bits, 0, sizeof(bits));
    memcpy(bits, data, (size_t)data_cw);
    memcpy(bits + data_cw, ec, (size_t)ec_len);
    nbits = (data_cw + ec_len) * 8;

    memset(fn, 0, sizeof(fn));
    memset(modules, 0, (size_t)(size * size));
    finder(modules, fn, size, 0, 0);
    finder(modules, fn, size, size - 7, 0);
    finder(modules, fn, size, 0, size - 7);
    timing_and_dark(modules, fn, size);
    alignment(modules, fn, size, ver);
    reserve_format(fn, size);

    for (int mask = 0; mask < 8; mask++) {
        memcpy(trial, modules, (size_t)(size * size));
        place_data(trial, fn, size, bits, nbits, mask);
        write_format(trial, size, mask);
        int pen = penalty(trial, size);
        if (pen < best_pen) {
            best_pen = pen;
            best_mask = mask;
        }
    }
    place_data(modules, fn, size, bits, nbits, best_mask);
    write_format(modules, size, best_mask);
    if (out_size) {
        *out_size = size;
    }
    return size;
}
