/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Non-functional stand-in for the parts of OpenSSL that psec_ssl.c uses,
 * so the component can be compile-checked by --enable-test-build on a
 * host without OpenSSL. Every call fails: X509_STORE_new() and the PEM
 * readers return NULL, so ssl_init() cannot succeed and the psec
 * framework never selects a module built against this header. Never
 * include it in a real build.
 */

#ifndef PMIX_PSEC_SSL_TESTBUILD_H
#define PMIX_PSEC_SSL_TESTBUILD_H

#include <stddef.h>

typedef struct pmix_tb_evp_pkey EVP_PKEY;
typedef struct pmix_tb_evp_md EVP_MD;
typedef struct pmix_tb_evp_md_ctx EVP_MD_CTX;
typedef struct pmix_tb_engine ENGINE;
typedef struct pmix_tb_bio BIO;
typedef struct pmix_tb_bio_method BIO_METHOD;
typedef struct pmix_tb_x509 X509;
typedef struct pmix_tb_x509_name X509_NAME;
typedef struct pmix_tb_x509_name_entry X509_NAME_ENTRY;
typedef struct pmix_tb_x509_store X509_STORE;
typedef struct pmix_tb_x509_store_ctx X509_STORE_CTX;
typedef struct pmix_tb_x509_lookup X509_LOOKUP;
typedef struct pmix_tb_x509_lookup_method X509_LOOKUP_METHOD;
typedef struct pmix_tb_asn1_string ASN1_STRING;
typedef int pem_password_cb(char *buf, int size, int rwflag, void *u);

#define STACK_OF(type) struct pmix_tb_stack_##type
STACK_OF(X509);

#define EVP_PKEY_ED25519        1087
#define EVP_PKEY_ED448          1088
#define NID_commonName          13
#define X509_FILETYPE_PEM       1
#define X509_V_FLAG_CRL_CHECK   0x4
#define X509_PURPOSE_SSL_CLIENT 1

static inline unsigned long ERR_get_error(void) { return 0; }
static inline void ERR_clear_error(void) { }
static inline void ERR_error_string_n(unsigned long e, char *buf, size_t len)
{
    (void) e;
    if (0 < len) {
        buf[0] = '\0';
    }
}

static inline void OPENSSL_cleanse(void *ptr, size_t len) { (void) ptr; (void) len; }
static inline int RAND_bytes(unsigned char *buf, int num) { (void) buf; (void) num; return 0; }

static inline int EVP_PKEY_id(const EVP_PKEY *pkey) { (void) pkey; return 0; }
static inline void EVP_PKEY_free(EVP_PKEY *pkey) { (void) pkey; }
static inline const EVP_MD *EVP_sha256(void) { return NULL; }
static inline EVP_MD_CTX *EVP_MD_CTX_new(void) { return NULL; }
static inline void EVP_MD_CTX_free(EVP_MD_CTX *ctx) { (void) ctx; }
static inline int EVP_DigestSignInit(EVP_MD_CTX *ctx, void **pctx, const EVP_MD *type,
                                     ENGINE *e, EVP_PKEY *pkey)
{
    (void) ctx; (void) pctx; (void) type; (void) e; (void) pkey;
    return 0;
}
static inline int EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen,
                                 const unsigned char *tbs, size_t tbslen)
{
    (void) ctx; (void) sig; (void) siglen; (void) tbs; (void) tbslen;
    return 0;
}
static inline int EVP_DigestVerifyInit(EVP_MD_CTX *ctx, void **pctx, const EVP_MD *type,
                                       ENGINE *e, EVP_PKEY *pkey)
{
    (void) ctx; (void) pctx; (void) type; (void) e; (void) pkey;
    return 0;
}
static inline int EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen,
                                   const unsigned char *tbs, size_t tbslen)
{
    (void) ctx; (void) sig; (void) siglen; (void) tbs; (void) tbslen;
    return 0;
}

static inline BIO *BIO_new_mem_buf(const void *buf, int len) { (void) buf; (void) len; return NULL; }
static inline const BIO_METHOD *BIO_s_mem(void) { return NULL; }
static inline BIO *BIO_new(const BIO_METHOD *type) { (void) type; return NULL; }
static inline int BIO_free(BIO *a) { (void) a; return 0; }
static inline long BIO_get_mem_data(BIO *b, char **pp) { (void) b; *pp = NULL; return 0; }

static inline EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x, pem_password_cb *cb,
                                                void *u)
{
    (void) bp; (void) x; (void) cb; (void) u;
    return NULL;
}
static inline X509 *PEM_read_bio_X509(BIO *bp, X509 **x, pem_password_cb *cb, void *u)
{
    (void) bp; (void) x; (void) cb; (void) u;
    return NULL;
}
static inline int PEM_write_bio_X509(BIO *bp, X509 *x) { (void) bp; (void) x; return 0; }

static inline void X509_free(X509 *a) { (void) a; }
static inline int X509_check_private_key(const X509 *x, const EVP_PKEY *k)
{
    (void) x; (void) k;
    return 0;
}
static inline X509_NAME *X509_get_subject_name(const X509 *a) { (void) a; return NULL; }
static inline int X509_NAME_get_index_by_NID(const X509_NAME *name, int nid, int lastpos)
{
    (void) name; (void) nid; (void) lastpos;
    return -1;
}
static inline X509_NAME_ENTRY *X509_NAME_get_entry(const X509_NAME *name, int loc)
{
    (void) name; (void) loc;
    return NULL;
}
static inline ASN1_STRING *X509_NAME_ENTRY_get_data(const X509_NAME_ENTRY *ne)
{
    (void) ne;
    return NULL;
}
static inline int ASN1_STRING_length(const ASN1_STRING *x) { (void) x; return 0; }
static inline const unsigned char *ASN1_STRING_get0_data(const ASN1_STRING *x)
{
    (void) x;
    return NULL;
}
static inline EVP_PKEY *X509_get0_pubkey(const X509 *x) { (void) x; return NULL; }

static inline STACK_OF(X509) *sk_X509_new_null(void) { return NULL; }
static inline int sk_X509_push(STACK_OF(X509) *sk, X509 *x) { (void) sk; (void) x; return 0; }
static inline void sk_X509_pop_free(STACK_OF(X509) *sk, void (*fn)(X509 *))
{
    (void) sk; (void) fn;
}

static inline X509_STORE *X509_STORE_new(void) { return NULL; }
static inline void X509_STORE_free(X509_STORE *s) { (void) s; }
static inline int X509_STORE_set_flags(X509_STORE *s, unsigned long flags)
{
    (void) s; (void) flags;
    return 0;
}
static inline X509_LOOKUP_METHOD *X509_LOOKUP_file(void) { return NULL; }
static inline X509_LOOKUP *X509_STORE_add_lookup(X509_STORE *s, X509_LOOKUP_METHOD *m)
{
    (void) s; (void) m;
    return NULL;
}
static inline int X509_load_cert_crl_file(X509_LOOKUP *ctx, const char *file, int type)
{
    (void) ctx; (void) file; (void) type;
    return 0;
}
static inline int X509_load_crl_file(X509_LOOKUP *ctx, const char *file, int type)
{
    (void) ctx; (void) file; (void) type;
    return 0;
}
static inline X509_STORE_CTX *X509_STORE_CTX_new(void) { return NULL; }
static inline void X509_STORE_CTX_free(X509_STORE_CTX *ctx) { (void) ctx; }
static inline int X509_STORE_CTX_init(X509_STORE_CTX *ctx, X509_STORE *store, X509 *x509,
                                      STACK_OF(X509) *chain)
{
    (void) ctx; (void) store; (void) x509; (void) chain;
    return 0;
}
static inline int X509_STORE_CTX_set_purpose(X509_STORE_CTX *ctx, int purpose)
{
    (void) ctx; (void) purpose;
    return 0;
}
static inline int X509_verify_cert(X509_STORE_CTX *ctx) { (void) ctx; return 0; }
static inline int X509_STORE_CTX_get_error(const X509_STORE_CTX *ctx) { (void) ctx; return 0; }
static inline const char *X509_verify_cert_error_string(long n)
{
    (void) n;
    return "OpenSSL testbuild shim - not functional";
}

#endif /* PMIX_PSEC_SSL_TESTBUILD_H */
