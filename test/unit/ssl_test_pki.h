/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* A throwaway PKI for the tests of psec/ssl: keys, certificates signed by
 * a CA made on the spot, and the files the component reads them from.
 * Every function returns what OpenSSL gives back and checks nothing - a
 * test that cannot mint its PKI fails on its first credential. */

#ifndef PMIX_TEST_SSL_PKI_H
#define PMIX_TEST_SSL_PKI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

static inline EVP_PKEY *gen_key(int type)
{
    EVP_PKEY_CTX *c;
    EVP_PKEY *k = NULL;

    c = EVP_PKEY_CTX_new_id(type, NULL);
    if (NULL == c || 1 != EVP_PKEY_keygen_init(c)) {
        EVP_PKEY_CTX_free(c);
        return NULL;
    }
    if (EVP_PKEY_EC == type &&
        1 != EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c, NID_X9_62_prime256v1)) {
        EVP_PKEY_CTX_free(c);
        return NULL;
    }
    if (1 != EVP_PKEY_keygen(c, &k)) {
        k = NULL;
    }
    EVP_PKEY_CTX_free(c);
    return k;
}

static inline const EVP_MD *md_for(EVP_PKEY *k)
{
    return (EVP_PKEY_ED25519 == EVP_PKEY_id(k)) ? NULL : EVP_sha256();
}

static inline void add_ext(X509 *x, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509_EXTENSION *ext;

    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, x, NULL, NULL, 0);
    ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
    if (NULL != ext) {
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }
}

/* issuer NULL: self-signed. eku NULL: a CA */
static inline X509 *make_cert(EVP_PKEY *key, const char *cn, X509 *issuer, EVP_PKEY *issuer_key,
                       long serial, long from, long until, const char *eku)
{
    X509 *x = X509_new();
    X509_NAME *n;

    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
    X509_gmtime_adj(X509_getm_notBefore(x), from);
    X509_gmtime_adj(X509_getm_notAfter(x), until);
    X509_set_pubkey(x, key);
    n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *) cn, -1, -1, 0);
    X509_set_issuer_name(x, (NULL == issuer) ? n : X509_get_subject_name(issuer));
    if (NULL == eku) {
        add_ext(x, (NULL == issuer) ? x : issuer, NID_basic_constraints, "critical,CA:TRUE");
        add_ext(x, (NULL == issuer) ? x : issuer, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add_ext(x, issuer, NID_basic_constraints, "CA:FALSE");
        add_ext(x, issuer, NID_key_usage, "critical,digitalSignature");
        add_ext(x, issuer, NID_ext_key_usage, eku);
    }
    if (NULL == issuer_key) {
        issuer_key = key;
    }
    X509_sign(x, issuer_key, md_for(issuer_key));
    return x;
}

/* PEM text of one or two certificates, leaf first */
static inline char *chain_pem(X509 *leaf, X509 *intermediate, size_t *len)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *data, *out;
    long n;

    PEM_write_bio_X509(b, leaf);
    if (NULL != intermediate) {
        PEM_write_bio_X509(b, intermediate);
    }
    n = BIO_get_mem_data(b, &data);
    out = (char *) malloc((size_t) n);
    memcpy(out, data, (size_t) n);
    *len = (size_t) n;
    BIO_free(b);
    return out;
}

static inline void write_file(const char *path, const char *data, size_t len, mode_t mode)
{
    FILE *fp = fopen(path, "w");

    fwrite(data, 1, len, fp);
    fclose(fp);
    chmod(path, mode);
}

static inline void write_key(const char *path, EVP_PKEY *k, mode_t mode)
{
    FILE *fp = fopen(path, "w");

    PEM_write_PrivateKey(fp, k, NULL, NULL, 0, NULL, NULL);
    fclose(fp);
    chmod(path, mode);
}

#endif
