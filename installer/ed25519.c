/* Ed25519 signature verification (RFC 8032, section 5.1.7), written for the launcher's update check.
 *
 * Only verification is needed here: releases are signed by scripts/make_release.py (scripts/ed25519.py).
 * All inputs are public (the release manifest, its signature and the public key built into the program),
 * so this code does not need to be constant-time. SHA-512 comes from Windows (bcrypt).
 *
 * Field elements mod p = 2^255 - 19 are 16 signed 64-bit limbs of 16 bits each (value = sum l[i] 2^(16 i));
 * 2^256 = 38 (mod p) folds the top. Points are extended coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, xy = T/Z.
 * The curve constants are derived at first use from their definitions (d = -121665/121666, sqrt(-1) =
 * 2^((p-1)/4), the base point from its standard encoding 0x58 0x66...), so no table of magic numbers is needed.
 */
#include "ed25519.h"

#include <string.h>
#include <windows.h>
#include <bcrypt.h>

typedef int64_t fe[16];

static void fe_copy(fe o, const fe a) { memcpy(o, a, sizeof(fe)); }
static void fe_set(fe o, int64_t v) { memset(o, 0, sizeof(fe)); o[0] = v; }

static void fe_carry(fe o) {
    for (int i = 0; i < 16; i++) {
        int64_t c = o[i] >> 16;                     /* floor division (arithmetic shift) */
        o[i] -= c * 65536;
        if (i < 15) o[i + 1] += c; else o[0] += 38 * c;
    }
}

static void fe_add(fe o, const fe a, const fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(fe o, const fe a, const fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fe_mul(fe o, const fe a, const fe b) {
    int64_t t[31];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* a^e for e given as big-endian bits from bit `top` down: generic square-and-multiply */
static void fe_pow(fe o, const fe a, const uint8_t e[32]) {   /* e little-endian bytes */
    fe r, b;
    fe_set(r, 1);
    fe_copy(b, a);
    for (int i = 255; i >= 0; i--) {
        fe_sq(r, r);
        if ((e[i >> 3] >> (i & 7)) & 1) fe_mul(r, r, b);
    }
    fe_copy(o, r);
}

/* canonical 32-byte little-endian encoding (value reduced below p) */
static void fe_pack(uint8_t out[32], const fe a) {
    fe t;
    fe_copy(t, a);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);                                    /* now 0 <= limbs < 2^16, value < 2^256 < 3p */
    for (int pass = 0; pass < 2; pass++) {          /* subtract p while value >= p */
        fe m;
        int64_t borrow = 0;
        for (int i = 0; i < 16; i++) {
            int64_t pl = i == 0 ? 0xffed : i == 15 ? 0x7fff : 0xffff;
            m[i] = t[i] - pl - borrow;
            borrow = m[i] < 0;
            if (borrow) m[i] += 65536;
        }
        if (!borrow) fe_copy(t, m);                 /* no borrow out of the top: t >= p */
    }
    for (int i = 0; i < 16; i++) { out[2 * i] = (uint8_t)(t[i] & 0xff); out[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}

static void fe_unpack(fe o, const uint8_t in[32]) {
    for (int i = 0; i < 16; i++) o[i] = in[2 * i] | (int64_t)in[2 * i + 1] << 8;
    o[15] &= 0x7fff;
}

static int fe_equal(const fe a, const fe b) {
    uint8_t x[32], y[32];
    fe_pack(x, a);
    fe_pack(y, b);
    return !memcmp(x, y, 32);
}

static int fe_parity(const fe a) {
    uint8_t x[32];
    fe_pack(x, a);
    return x[0] & 1;
}

/* exponents (little-endian): p - 2 (inverse), (p - 5) / 8, (p - 1) / 4 */
static uint8_t E_INV[32], E_P58[32], E_P14[32];

static void fe_inv(fe o, const fe a) { fe_pow(o, a, E_INV); }

/* ---- the curve ---------------------------------------------------------------------------------- */

typedef struct { fe x, y, z, t; } ge;

static fe D, D2, SQRTM1;
static ge B;
static int ready;

/* P = P + Q (RFC 8032 5.1.4, a = -1; also correct for P == Q) */
static void ge_add(ge* p, const ge* q) {
    fe a, b, c, d, t, e, f, g, h;
    fe_sub(a, p->y, p->x);
    fe_sub(t, q->y, q->x);
    fe_mul(a, a, t);
    fe_add(b, p->y, p->x);
    fe_add(t, q->y, q->x);
    fe_mul(b, b, t);
    fe_mul(c, p->t, q->t);
    fe_mul(c, c, D2);
    fe_mul(d, p->z, q->z);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(p->x, e, f);
    fe_mul(p->y, h, g);
    fe_mul(p->z, g, f);
    fe_mul(p->t, e, h);
}

static void ge_zero(ge* p) { fe_set(p->x, 0); fe_set(p->y, 1); fe_set(p->z, 1); fe_set(p->t, 0); }

/* r = s * q, s a 256-bit little-endian scalar (variable time: public data only) */
static void ge_scalarmult(ge* r, const ge* q, const uint8_t s[32]) {
    ge acc;
    ge_zero(&acc);
    for (int i = 255; i >= 0; i--) {
        ge dbl = acc;
        ge_add(&acc, &dbl);
        if ((s[i >> 3] >> (i & 7)) & 1) ge_add(&acc, q);
    }
    *r = acc;
}

static void ge_pack(uint8_t out[32], const ge* p) {
    fe zi, x, y;
    fe_inv(zi, p->z);
    fe_mul(x, p->x, zi);
    fe_mul(y, p->y, zi);
    fe_pack(out, y);
    out[31] ^= (uint8_t)(fe_parity(x) << 7);
}

/* RFC 8032 5.1.3: decode a point; 0 if the encoding is not a valid, canonical point */
static int ge_unpack(ge* p, const uint8_t in[32]) {
    uint8_t chk[32];
    fe_unpack(p->y, in);
    fe_pack(chk, p->y);                             /* y must be canonical (< p) */
    for (int i = 0; i < 32; i++)
        if (chk[i] != (i == 31 ? (in[31] & 0x7f) : in[i])) return 0;
    int sign = in[31] >> 7;
    fe one, u, v, v3, x, t;
    fe_set(one, 1);
    fe_set(p->z, 1);
    fe_sq(u, p->y);
    fe_mul(v, u, D);
    fe_sub(u, u, one);                              /* u = y^2 - 1 */
    fe_add(v, v, one);                              /* v = d y^2 + 1 */
    fe_sq(v3, v);
    fe_mul(v3, v3, v);                              /* v^3 */
    fe_sq(t, v3);
    fe_mul(t, t, v);                                /* v^7 */
    fe_mul(t, t, u);                                /* u v^7 */
    fe_pow(t, t, E_P58);                            /* (u v^7)^((p-5)/8) */
    fe_mul(x, t, u);
    fe_mul(x, x, v3);                               /* x = u v^3 (u v^7)^((p-5)/8) */
    fe_sq(t, x);
    fe_mul(t, t, v);                                /* v x^2 */
    if (!fe_equal(t, u)) {
        fe neg;
        fe_set(neg, 0);
        fe_sub(neg, neg, u);
        if (!fe_equal(t, neg)) return 0;            /* no square root: not on the curve */
        fe_mul(x, x, SQRTM1);
    }
    fe zero;
    fe_set(zero, 0);
    if (fe_equal(x, zero) && sign) return 0;
    if (fe_parity(x) != sign) fe_sub(x, zero, x);
    fe_copy(p->x, x);
    fe_mul(p->t, p->x, p->y);
    return 1;
}

/* little-endian 256-bit constant: 2^255 - 19 - k */
static void p_minus(uint8_t out[32], int k) {
    memset(out, 0xff, 32);
    out[31] = 0x7f;
    out[0] = (uint8_t)(0xed - k);                   /* p = ...ffed */
}

static void shift_right(uint8_t v[32], int n) {   /* v >>= n (n < 8) */
    for (int i = 0; i < 32; i++) v[i] = (uint8_t)((v[i] >> n) | (i < 31 ? v[i + 1] << (8 - n) : 0));
}

static void init(void) {
    if (ready) return;
    p_minus(E_INV, 2);                              /* p - 2 */
    p_minus(E_P58, 5);
    shift_right(E_P58, 3);                          /* (p - 5) / 8 */
    p_minus(E_P14, 1);
    shift_right(E_P14, 2);                          /* (p - 1) / 4 */
    fe a, b;
    fe_set(a, 121665);
    fe_set(b, 121666);
    fe_inv(b, b);
    fe_mul(D, a, b);
    fe zero;
    fe_set(zero, 0);
    fe_sub(D, zero, D);                             /* d = -121665 / 121666 */
    fe_add(D2, D, D);
    fe two;
    fe_set(two, 2);
    fe_pow(SQRTM1, two, E_P14);                     /* 2^((p-1)/4) = sqrt(-1) */
    uint8_t enc[32];
    memset(enc, 0x66, 32);
    enc[0] = 0x58;                                  /* the base point: y = 4/5, x even */
    ge_unpack(&B, enc);
    ready = 1;
}

/* ---- scalars mod L = 2^252 + 27742317777372353535851937790883648493 -------------------------------- */

static const uint8_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static int ge_l(const uint8_t* a) {               /* a (32 bytes LE) >= L ? */
    for (int i = 31; i >= 0; i--) {
        if (a[i] != L[i]) return a[i] > L[i];
    }
    return 1;
}

/* r = h (64 bytes LE) mod L, by shift-and-subtract (512 steps; r < L < 2^253 so 2r + 1 fits 33 bytes) */
static void reduce_l(uint8_t r[32], const uint8_t h[64]) {
    uint8_t acc[33];
    memset(acc, 0, sizeof acc);
    for (int bit = 511; bit >= 0; bit--) {
        int carry = (h[bit >> 3] >> (bit & 7)) & 1;
        for (int i = 0; i < 33; i++) {
            int v = (acc[i] << 1) | carry;
            carry = v >> 8;
            acc[i] = (uint8_t)v;
        }
        if (acc[32] || ge_l(acc)) {
            int borrow = 0;
            for (int i = 0; i < 33; i++) {
                int v = acc[i] - (i < 32 ? L[i] : 0) - borrow;
                borrow = v < 0;
                acc[i] = (uint8_t)(v + (borrow ? 256 : 0));
            }
        }
    }
    memcpy(r, acc, 32);
}

/* ---- SHA-512 (Windows CNG) ------------------------------------------------------------------------ */

int ed25519_sha512(const void* a, size_t na, const void* b, size_t nb, const void* c, size_t nc, uint8_t out[64]) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, NULL, 0)) return 0;
    int ok = !BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
    if (ok) {
        if (na) ok &= !BCryptHashData(h, (PUCHAR)a, (ULONG)na, 0);
        if (nb) ok &= !BCryptHashData(h, (PUCHAR)b, (ULONG)nb, 0);
        if (nc) ok &= !BCryptHashData(h, (PUCHAR)c, (ULONG)nc, 0);
        ok &= !BCryptFinishHash(h, out, 64, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ---- verify ----------------------------------------------------------------------------------------- */

int ed25519_verify(const uint8_t sig[64], const void* msg, size_t len, const uint8_t pub[32]) {
    init();
    if (ge_l(sig + 32)) return 0;                   /* S must be < L (no malleable signatures) */
    ge a;
    if (!ge_unpack(&a, pub)) return 0;
    uint8_t h[64], k[32];
    if (!ed25519_sha512(sig, 32, pub, 32, msg, len, h)) return 0;
    reduce_l(k, h);
    /* check [S]B == R + [k]A, as [S]B + [k](-A) encoded == R */
    fe zero;
    fe_set(zero, 0);
    fe_sub(a.x, zero, a.x);
    fe_sub(a.t, zero, a.t);
    ge sb, ka;
    ge_scalarmult(&sb, &B, sig + 32);
    ge_scalarmult(&ka, &a, k);
    ge_add(&sb, &ka);
    uint8_t enc[32];
    ge_pack(enc, &sb);
    return !memcmp(enc, sig, 32);
}

int ed25519_from_hex(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}
