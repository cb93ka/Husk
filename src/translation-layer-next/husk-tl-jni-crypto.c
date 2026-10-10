/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Java's cryptography, as .NET for Android reaches it: hashes (MessageDigest), HMAC (Mac), block ciphers (Cipher, with
 * SecretKeySpec and IvParameterSpec) and SecureRandom, done with the system's CommonCrypto.
 *
 * .NET's System.Security.Cryptography on Android has no native crypto of its own: libSystem.Security.Cryptography.Native.Android
 * calls these Java classes through JNI. Without them a game's hashing or decryption fails ("Error occurred during a
 * cryptographic operation") -- PvZ Hybrid Remake stopped loading there. AES-GCM, RSA and the rest are not offered.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void tl_log_line(const char *fmt, ...);

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }

static jobj *bytes(const void *p, size_t n)
{
    jobj *a = tl_jni_new_prim_array('B', (uint32_t)n);
    if (n) memcpy(a->arr.data, p, n);
    return a;
}
static const uint8_t *arr_data(const jobj *a) { return a && a->kind == TL_K_PRIM_ARRAY ? a->arr.data : NULL; }
static size_t arr_len(const jobj *a) { return a && a->kind == TL_K_PRIM_ARRAY ? a->arr.len : 0; }
static jobj *make(const char *cls) { return tl_jni_new_object(tl_jni_class(cls)); }

/* ------------------------------------------------------------- MessageDigest */

enum { MD5, SHA1, SHA256, SHA384, SHA512 };
typedef struct {
    int alg;
    union { CC_MD5_CTX md5; CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; CC_SHA512_CTX sha512; } u;
} digest;

static int digest_alg(const char *name)
{
    if (!name) return -1;
    if (!strcasecmp(name, "MD5")) return MD5;
    if (!strcasecmp(name, "SHA-1") || !strcasecmp(name, "SHA1")) return SHA1;
    if (!strcasecmp(name, "SHA-256") || !strcasecmp(name, "SHA256")) return SHA256;
    if (!strcasecmp(name, "SHA-384") || !strcasecmp(name, "SHA384")) return SHA384;
    if (!strcasecmp(name, "SHA-512") || !strcasecmp(name, "SHA512")) return SHA512;
    return -1;
}
static void digest_init(digest *d)
{
    switch (d->alg) {
    case MD5: CC_MD5_Init(&d->u.md5); break;
    case SHA1: CC_SHA1_Init(&d->u.sha1); break;
    case SHA256: CC_SHA256_Init(&d->u.sha256); break;
    case SHA384: CC_SHA384_Init(&d->u.sha512); break;
    default: CC_SHA512_Init(&d->u.sha512); break;
    }
}
static void digest_update(digest *d, const void *p, size_t n)
{
    switch (d->alg) {
    case MD5: CC_MD5_Update(&d->u.md5, p, (CC_LONG)n); break;
    case SHA1: CC_SHA1_Update(&d->u.sha1, p, (CC_LONG)n); break;
    case SHA256: CC_SHA256_Update(&d->u.sha256, p, (CC_LONG)n); break;
    case SHA384: CC_SHA384_Update(&d->u.sha512, p, (CC_LONG)n); break;
    default: CC_SHA512_Update(&d->u.sha512, p, (CC_LONG)n); break;
    }
}
/* The hash so far, and the digest reset for more input (as Java's digest() leaves it). */
static jobj *digest_final(digest *d)
{
    uint8_t out[64]; size_t n;
    switch (d->alg) {
    case MD5: CC_MD5_Final(out, &d->u.md5); n = 16; break;
    case SHA1: CC_SHA1_Final(out, &d->u.sha1); n = 20; break;
    case SHA256: CC_SHA256_Final(out, &d->u.sha256); n = 32; break;
    case SHA384: CC_SHA384_Final(out, &d->u.sha512); n = 48; break;
    default: CC_SHA512_Final(out, &d->u.sha512); n = 64; break;
    }
    digest_init(d);
    return bytes(out, n);
}

static void MD_getInstance(tl_jcall *c)
{
    const char *name = tl_jni_string(c->args[0].l);
    int alg = digest_alg(name);
    if (alg < 0) { tl_jni_throw("java/security/NoSuchAlgorithmException", name ? name : ""); c->ret = vl(NULL); return; }
    jobj *o = make("java/security/MessageDigest");
    digest *d = calloc(1, sizeof(*d));
    d->alg = alg; digest_init(d);
    o->native = d;
    c->ret = vl(o);
}
static void MD_update(tl_jcall *c) { digest *d = c->self->native; if (d) digest_update(d, arr_data(c->args[0].l), arr_len(c->args[0].l)); }
static void MD_digest(tl_jcall *c) { digest *d = c->self->native; c->ret = vl(d ? digest_final(d) : NULL); }
static void MD_digestWith(tl_jcall *c) { MD_update(c); MD_digest(c); }
static void MD_reset(tl_jcall *c) { digest *d = c->self->native; if (d) digest_init(d); }
static void MD_clone(tl_jcall *c)
{
    digest *d = c->self->native;
    jobj *o = make("java/security/MessageDigest");
    if (d) { digest *n = malloc(sizeof(*n)); memcpy(n, d, sizeof(*n)); o->native = n; }
    c->ret = vl(o);
}

/* ---------------------------------------------------------------- key, IV */

typedef struct { uint8_t *p; size_t n; int tag_bits; } blob;
static blob *blob_of(const jobj *a)
{
    blob *b = calloc(1, sizeof(*b));
    b->n = arr_len(a);
    b->p = malloc(b->n ? b->n : 1);
    if (b->n) memcpy(b->p, arr_data(a), b->n);
    return b;
}
static void SKS_init(tl_jcall *c) { c->self->native = blob_of(c->args[0].l); }
static void IV_init(tl_jcall *c) { c->self->native = blob_of(c->args[0].l); }
static void GCM_init(tl_jcall *c) { blob *b = blob_of(c->args[1].l); b->tag_bits = c->args[0].i; c->self->native = b; }

/* ---------------------------------------------------------------------- Mac */

typedef struct { CCHmacAlgorithm alg; size_t len; CCHmacContext ctx; uint8_t key[256]; size_t keylen; } hmac;

static void Mac_getInstance(tl_jcall *c)
{
    const char *name = tl_jni_string(c->args[0].l);
    CCHmacAlgorithm alg; size_t len;
    if (name && !strcasecmp(name, "HmacMD5")) { alg = kCCHmacAlgMD5; len = 16; }
    else if (name && !strcasecmp(name, "HmacSHA1")) { alg = kCCHmacAlgSHA1; len = 20; }
    else if (name && !strcasecmp(name, "HmacSHA256")) { alg = kCCHmacAlgSHA256; len = 32; }
    else if (name && !strcasecmp(name, "HmacSHA384")) { alg = kCCHmacAlgSHA384; len = 48; }
    else if (name && !strcasecmp(name, "HmacSHA512")) { alg = kCCHmacAlgSHA512; len = 64; }
    else { tl_jni_throw("java/security/NoSuchAlgorithmException", name ? name : ""); c->ret = vl(NULL); return; }
    jobj *o = make("javax/crypto/Mac");
    hmac *h = calloc(1, sizeof(*h));
    h->alg = alg; h->len = len;
    o->native = h;
    c->ret = vl(o);
}
static void Mac_init(tl_jcall *c)
{
    hmac *h = c->self->native; const blob *k = c->args[0].l ? ((jobj *)c->args[0].l)->native : NULL;
    if (!h) return;
    h->keylen = k ? (k->n < sizeof(h->key) ? k->n : sizeof(h->key)) : 0;
    if (h->keylen) memcpy(h->key, k->p, h->keylen);
    CCHmacInit(&h->ctx, h->alg, h->key, h->keylen);
}
static void Mac_update(tl_jcall *c) { hmac *h = c->self->native; if (h) CCHmacUpdate(&h->ctx, arr_data(c->args[0].l), arr_len(c->args[0].l)); }
static void Mac_doFinal(tl_jcall *c)
{
    hmac *h = c->self->native;
    if (!h) { c->ret = vl(NULL); return; }
    uint8_t out[64];
    CCHmacFinal(&h->ctx, out);
    CCHmacInit(&h->ctx, h->alg, h->key, h->keylen);         /* ready for the next message, with the same key */
    c->ret = vl(bytes(out, h->len));
}
static void Mac_reset(tl_jcall *c) { hmac *h = c->self->native; if (h) CCHmacInit(&h->ctx, h->alg, h->key, h->keylen); }
static void Mac_clone(tl_jcall *c)
{
    hmac *h = c->self->native;
    jobj *o = make("javax/crypto/Mac");
    if (h) { hmac *n = malloc(sizeof(*n)); memcpy(n, h, sizeof(*n)); o->native = n; }
    c->ret = vl(o);
}

/* ------------------------------------------------------------------- Cipher */

typedef struct {
    CCAlgorithm alg; CCMode mode; CCPadding pad; size_t block;
    CCCryptorRef ref;
    uint8_t iv[32]; bool has_iv;
} cipher;

static void Cipher_getInstance(tl_jcall *c)
{
    const char *t = tl_jni_string(c->args[0].l);
    char a[32] = "", m[32] = "", p[32] = "";
    if (t) sscanf(t, "%31[^/]/%31[^/]/%31s", a, m, p);
    cipher ci = { .mode = kCCModeECB, .pad = ccNoPadding };
    bool ok = true;
    if (!strcasecmp(a, "AES")) { ci.alg = kCCAlgorithmAES; ci.block = 16; }
    else if (!strcasecmp(a, "DES")) { ci.alg = kCCAlgorithmDES; ci.block = 8; }
    else if (!strcasecmp(a, "DESede") || !strcasecmp(a, "TripleDES")) { ci.alg = kCCAlgorithm3DES; ci.block = 8; }
    else if (!strcasecmp(a, "RC2")) { ci.alg = kCCAlgorithmRC2; ci.block = 8; }
    else ok = false;
    if (!m[0] || !strcasecmp(m, "ECB")) ci.mode = kCCModeECB;
    else if (!strcasecmp(m, "CBC")) ci.mode = kCCModeCBC;
    else if (!strcasecmp(m, "CFB") || !strcasecmp(m, "CFB128")) ci.mode = kCCModeCFB;
    else if (!strcasecmp(m, "CFB8")) ci.mode = kCCModeCFB8;
    else ok = false;                                         /* GCM, CCM and the rest are not offered */
    if (!strcasecmp(p, "PKCS5Padding") || !strcasecmp(p, "PKCS7Padding")) ci.pad = ccPKCS7Padding;
    if (!ok) {
        tl_log_line("crypto: cipher %s is not offered", t ? t : "?");
        tl_jni_throw("java/security/NoSuchAlgorithmException", t ? t : "");
        c->ret = vl(NULL);
        return;
    }
    jobj *o = make("javax/crypto/Cipher");
    cipher *n = malloc(sizeof(*n)); *n = ci;
    o->native = n;
    c->ret = vl(o);
}
static void cipher_init(tl_jcall *c, const blob *iv)
{
    cipher *ci = c->self->native;
    const blob *k = c->args[1].l ? ((jobj *)c->args[1].l)->native : NULL;
    if (!ci || !k) { tl_jni_throw("java/security/InvalidKeyException", "no key"); return; }
    if (ci->ref) { CCCryptorRelease(ci->ref); ci->ref = NULL; }
    ci->has_iv = iv && iv->n;
    memset(ci->iv, 0, sizeof(ci->iv));
    if (ci->has_iv) memcpy(ci->iv, iv->p, iv->n < sizeof(ci->iv) ? iv->n : sizeof(ci->iv));
    CCOperation op = c->args[0].i == 2 /* DECRYPT_MODE */ ? kCCDecrypt : kCCEncrypt;
    CCCryptorStatus st = CCCryptorCreateWithMode(op, ci->mode, ci->alg, ci->pad, ci->has_iv ? ci->iv : NULL, k->p, k->n, NULL, 0, 0, 0, &ci->ref);
    if (st != kCCSuccess) { tl_log_line("crypto: cipher init failed (%d, key %zu bytes)", (int)st, k->n); tl_jni_throw("java/security/InvalidKeyException", "init failed"); }
}
static void Cipher_init(tl_jcall *c) { cipher_init(c, NULL); }
static void Cipher_initParams(tl_jcall *c)
{
    const jobj *spec = c->args[2].l;
    if (spec && spec->cls && strstr(tl_jni_class_name((jobj *)spec), "GCMParameterSpec")) {
        tl_jni_throw("java/security/InvalidAlgorithmParameterException", "GCM is not offered");
        return;
    }
    cipher_init(c, spec ? spec->native : NULL);
}
static jobj *cipher_run(cipher *ci, const uint8_t *in, size_t n, bool final)
{
    if (!ci || !ci->ref) return NULL;
    size_t cap = CCCryptorGetOutputLength(ci->ref, n, final) + ci->block, moved = 0, more = 0;
    uint8_t *out = malloc(cap ? cap : 1);
    if (n) CCCryptorUpdate(ci->ref, in, n, out, cap, &moved);
    if (final) {
        CCCryptorFinal(ci->ref, out + moved, cap - moved, &more);
        CCCryptorReset(ci->ref, ci->has_iv ? ci->iv : NULL);   /* as after init, which is where Java leaves a cipher */
    }
    jobj *r = bytes(out, moved + more);
    free(out);
    return r;
}
static void Cipher_update(tl_jcall *c) { c->ret = vl(cipher_run(c->self->native, arr_data(c->args[0].l), arr_len(c->args[0].l), false)); }
static void Cipher_doFinal(tl_jcall *c) { c->ret = vl(cipher_run(c->self->native, NULL, 0, true)); }
static void Cipher_doFinalWith(tl_jcall *c) { c->ret = vl(cipher_run(c->self->native, arr_data(c->args[0].l), arr_len(c->args[0].l), true)); }
static void Cipher_getBlockSize(tl_jcall *c) { cipher *ci = c->self->native; c->ret = vi(ci ? (int)ci->block : 0); }
static void Cipher_updateAAD(tl_jcall *c) { (void)c; tl_jni_throw("java/lang/IllegalStateException", "no AEAD"); }

/* ------------------------------------------------------------- SecureRandom */

static void Noop(tl_jcall *c) { (void)c; }
static void SR_nextBytes(tl_jcall *c) { jobj *a = c->args[0].l; if (arr_len(a)) arc4random_buf(a->arr.data, arr_len(a)); }

#define M(c, n, s, f) { c, n, s, f }
static const tl_jhle k_crypto_hle[] = {
    M("java/security/MessageDigest", "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;", MD_getInstance),
    M("java/security/MessageDigest", "update", "([B)V", MD_update),
    M("java/security/MessageDigest", "digest", "()[B", MD_digest),
    M("java/security/MessageDigest", "digest", "([B)[B", MD_digestWith),
    M("java/security/MessageDigest", "reset", "()V", MD_reset),
    M("java/security/MessageDigest", "clone", "()Ljava/lang/Object;", MD_clone),
    M("javax/crypto/spec/SecretKeySpec", "<init>", "([BLjava/lang/String;)V", SKS_init),
    M("javax/crypto/spec/IvParameterSpec", "<init>", "([B)V", IV_init),
    M("javax/crypto/spec/GCMParameterSpec", "<init>", "(I[B)V", GCM_init),
    M("javax/crypto/Mac", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;", Mac_getInstance),
    M("javax/crypto/Mac", "init", "(Ljava/security/Key;)V", Mac_init),
    M("javax/crypto/Mac", "update", "([B)V", Mac_update),
    M("javax/crypto/Mac", "doFinal", "()[B", Mac_doFinal),
    M("javax/crypto/Mac", "reset", "()V", Mac_reset),
    M("javax/crypto/Mac", "clone", "()Ljava/lang/Object;", Mac_clone),
    M("javax/crypto/Cipher", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;", Cipher_getInstance),
    M("javax/crypto/Cipher", "init", "(ILjava/security/Key;)V", Cipher_init),
    M("javax/crypto/Cipher", "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V", Cipher_initParams),
    M("javax/crypto/Cipher", "update", "([B)[B", Cipher_update),
    M("javax/crypto/Cipher", "doFinal", "()[B", Cipher_doFinal),
    M("javax/crypto/Cipher", "doFinal", "([B)[B", Cipher_doFinalWith),
    M("javax/crypto/Cipher", "getBlockSize", "()I", Cipher_getBlockSize),
    M("javax/crypto/Cipher", "updateAAD", "([B)V", Cipher_updateAAD),
    M("java/security/SecureRandom", "<init>", "()V", Noop),
    M("java/security/SecureRandom", "nextBytes", "([B)V", SR_nextBytes),
    { NULL, NULL, NULL, NULL }
};

void tl_crypto_install(void)
{
    static const char *const classes[] = { "java/security/MessageDigest", "javax/crypto/Mac", "javax/crypto/Cipher", "java/security/SecureRandom",
                                           "javax/crypto/spec/SecretKeySpec", "javax/crypto/spec/IvParameterSpec", "javax/crypto/spec/GCMParameterSpec" };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) tl_jni_declare(classes[i], "java/lang/Object");
    tl_jni_register_hle(k_crypto_hle);
}
