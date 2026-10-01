/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * X.509 certificate authentication for peers on other hosts.
 *
 * native authenticates a peer by asking this host's kernel who owns the
 * far end of the connection, which is only possible when the peer is on
 * this host. This component authenticates a peer with a certificate: the
 * credential is a statement - "I am uid U, gid G, at time T, and this is
 * a nonce N" - signed with the private key whose certificate travels with
 * it. The server checks that the certificate chains to a CA it trusts,
 * that the certificate names a local user whose uid is U, that the
 * signature is the certificate's, that T is recent and that N has not
 * been seen before.
 *
 * This authenticates the connection; it does not encrypt it. psec runs
 * only while a connection is being established and never touches the
 * traffic that follows - see docs/security.rst.
 *
 * The credential is a single self-contained blob, so it also serves
 * PMIx_Get_credential / PMIx_Validate_credential unchanged.
 *
 * Layout, all integers in network byte order:
 *
 *   "PMIXSSL1"                  8 bytes
 *   uid                         uint32
 *   gid                         uint32
 *   time (seconds since epoch)  uint64
 *   nonce                       16 bytes
 *   chain length                uint32
 *   chain                       PEM certificates, leaf first
 *   signature length            uint32
 *   signature                   over every byte before the signature length
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif

#if PMIX_TESTBUILD
#    include "testbuild_ssl.h"
#else
#    include <openssl/bio.h>
#    include <openssl/crypto.h>
#    include <openssl/err.h>
#    include <openssl/evp.h>
#    include <openssl/objects.h>
#    include <openssl/pem.h>
#    include <openssl/rand.h>
#    include <openssl/x509.h>
#    include <openssl/x509_vfy.h>
#    include <openssl/x509v3.h>
#endif

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/psec/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "psec_ssl.h"

static pmix_status_t ssl_init(void);
static void ssl_finalize(void);
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred);
static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred);

pmix_psec_module_t pmix_ssl_module = {.name = "ssl",
                                      .init = ssl_init,
                                      .finalize = ssl_finalize,
                                      .create_cred = create_cred,
                                      .validate_cred = validate_cred};

#define SSL_MAGIC     "PMIXSSL1"
#define SSL_MAGIC_LEN 8
#define SSL_NONCE_LEN 16
#define SSL_OFF_UID   (SSL_MAGIC_LEN)
#define SSL_OFF_GID   (SSL_OFF_UID + 4)
#define SSL_OFF_TIME  (SSL_OFF_GID + 4)
#define SSL_OFF_NONCE (SSL_OFF_TIME + 8)
#define SSL_OFF_CHAIN (SSL_OFF_NONCE + SSL_NONCE_LEN)
#define SSL_HDR_LEN   (SSL_OFF_CHAIN + 4)
/* bounds on what a peer may hand us - well inside PMIX_MAX_CRED_SIZE */
#define SSL_MAX_CHAIN (64 * 1024)
#define SSL_MAX_SIG   2048
#define SSL_MAX_CERTS 10
/* the longest private-key or certificate file we will read */
#define SSL_MAX_FILE  (256 * 1024)

/* Everything below is set in init and only read afterwards, except the
 * replay list, which has its own lock: PMIx_Validate_credential runs
 * validate_cred on whichever application thread called it. */
static EVP_PKEY *mykey = NULL;
static char *mychain = NULL;
static size_t mychainlen = 0;
static X509_STORE *trust = NULL;

typedef struct {
    pmix_list_item_t super;
    unsigned char nonce[SSL_NONCE_LEN];
    int64_t when;
} pmix_psec_ssl_seen_t;
static PMIX_CLASS_INSTANCE(pmix_psec_ssl_seen_t, pmix_list_item_t, NULL, NULL);

static pmix_list_t seen;
static bool seen_ready = false;
static pmix_mutex_t seenlock = PMIX_MUTEX_STATIC_INIT;

/* ------------------------------------------------------------------ */

static void put_u32(unsigned char *p, uint32_t v)
{
    v = htonl(v);
    memcpy(p, &v, sizeof(v));
}

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof(v));
    return ntohl(v);
}

static void put_u64(unsigned char *p, uint64_t v)
{
    put_u32(p, (uint32_t) (v >> 32));
    put_u32(p + 4, (uint32_t) (v & 0xffffffffULL));
}

static uint64_t get_u64(const unsigned char *p)
{
    return ((uint64_t) get_u32(p) << 32) | (uint64_t) get_u32(p + 4);
}

/* drain OpenSSL's (per-thread) error queue into our verbose output, so a
 * failure is explained where the verbosity asks for it and the next
 * call does not inherit a stale error */
static void drain_errors(const char *what)
{
    unsigned long e;
    char buf[256];

    while (0 != (e = ERR_get_error())) {
        ERR_error_string_n(e, buf, sizeof(buf));
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec:ssl: %s: %s", what, buf);
    }
}

/* OpenSSL prompts on the terminal for the passphrase of an encrypted key
 * unless it is given a callback that answers - a library must never do
 * that, so refuse instead */
static int no_passphrase(char *buf, int size, int rwflag, void *u)
{
    PMIX_HIDE_UNUSED_PARAMS(buf, size, rwflag, u);
    return 0;
}

/* Sign with SHA-256, except for the key types that hash internally and
 * refuse to be handed a digest */
static const EVP_MD *digest_for(EVP_PKEY *pkey)
{
    int id = EVP_PKEY_id(pkey);

    if (EVP_PKEY_ED25519 == id || EVP_PKEY_ED448 == id) {
        return NULL;
    }
    return EVP_sha256();
}

/* Read a whole file into memory. A private key must be owned by this
 * process's user and not accessible to group or others, as ssh requires */
static pmix_status_t read_file(const char *path, bool private_key, char **out, size_t *outlen)
{
    struct stat st;
    char *buf;
    ssize_t n;
    size_t got = 0;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (0 > fd) {
        pmix_show_help("help-psec-ssl.txt", "bad-file", true, path, strerror(errno));
        return PMIX_ERR_NOT_FOUND;
    }
    if (0 != fstat(fd, &st) || !S_ISREG(st.st_mode) || SSL_MAX_FILE < st.st_size) {
        pmix_show_help("help-psec-ssl.txt", "bad-file", true, path,
                       "not a regular file of reasonable size");
        close(fd);
        return PMIX_ERR_BAD_PARAM;
    }
    if (private_key && (0 != (st.st_mode & (S_IRWXG | S_IRWXO)) || st.st_uid != geteuid())) {
        pmix_show_help("help-psec-ssl.txt", "key-permissions", true, path, path);
        close(fd);
        return PMIX_ERR_BAD_PARAM;
    }
    buf = (char *) malloc((size_t) st.st_size + 1);
    if (NULL == buf) {
        close(fd);
        return PMIX_ERR_NOMEM;
    }
    while (got < (size_t) st.st_size) {
        n = read(fd, buf + got, (size_t) st.st_size - got);
        if (0 > n && EINTR == errno) {
            continue;
        }
        if (0 >= n) {
            break;
        }
        got += (size_t) n;
    }
    close(fd);
    buf[got] = '\0';
    *out = buf;
    *outlen = got;
    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */

static pmix_status_t load_identity(void)
{
    char *buf = NULL, *data;
    size_t len = 0;
    long dlen;
    BIO *in = NULL, *out = NULL;
    X509 *cert, *leaf = NULL;
    int ncerts = 0;
    pmix_status_t rc;

    /* the key */
    rc = read_file(pmix_psec_ssl_params.key_file, true, &buf, &len);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    in = BIO_new_mem_buf(buf, (int) len);
    if (NULL != in) {
        mykey = PEM_read_bio_PrivateKey(in, NULL, no_passphrase, NULL);
        BIO_free(in);
    }
    OPENSSL_cleanse(buf, len);
    free(buf);
    buf = NULL;
    if (NULL == mykey) {
        drain_errors("reading the private key");
        pmix_show_help("help-psec-ssl.txt", "bad-key", true, pmix_psec_ssl_params.key_file);
        return PMIX_ERR_BAD_PARAM;
    }

    /* the certificate chain - re-written from what parsed rather than
     * sent as read, so nothing but certificates goes on the wire */
    rc = read_file(pmix_psec_ssl_params.cert_file, false, &buf, &len);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    in = BIO_new_mem_buf(buf, (int) len);
    out = BIO_new(BIO_s_mem());
    if (NULL == in || NULL == out) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    while (NULL != (cert = PEM_read_bio_X509(in, NULL, no_passphrase, NULL))) {
        if (SSL_MAX_CERTS <= ncerts || 1 != PEM_write_bio_X509(out, cert)) {
            X509_free(cert);
            pmix_show_help("help-psec-ssl.txt", "bad-cert", true,
                           pmix_psec_ssl_params.cert_file, "too many certificates in the chain");
            rc = PMIX_ERR_BAD_PARAM;
            goto done;
        }
        if (NULL == leaf) {
            leaf = cert;
        } else {
            X509_free(cert);
        }
        ++ncerts;
    }
    /* reaching the end of the file is reported as an error too */
    ERR_clear_error();
    if (NULL == leaf) {
        pmix_show_help("help-psec-ssl.txt", "bad-cert", true, pmix_psec_ssl_params.cert_file,
                       "no certificate could be read from it");
        rc = PMIX_ERR_BAD_PARAM;
        goto done;
    }
    if (1 != X509_check_private_key(leaf, mykey)) {
        drain_errors("matching the key to the certificate");
        pmix_show_help("help-psec-ssl.txt", "bad-cert", true, pmix_psec_ssl_params.cert_file,
                       "its first certificate does not belong to the private key in key_file");
        rc = PMIX_ERR_BAD_PARAM;
        goto done;
    }
    dlen = BIO_get_mem_data(out, &data);
    if (0 >= dlen || SSL_MAX_CHAIN < dlen) {
        pmix_show_help("help-psec-ssl.txt", "bad-cert", true, pmix_psec_ssl_params.cert_file,
                       "the chain is larger than a credential may carry");
        rc = PMIX_ERR_BAD_PARAM;
        goto done;
    }
    mychain = (char *) malloc((size_t) dlen);
    if (NULL == mychain) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    memcpy(mychain, data, (size_t) dlen);
    mychainlen = (size_t) dlen;
    rc = PMIX_SUCCESS;

done:
    if (NULL != leaf) {
        X509_free(leaf);
    }
    if (NULL != in) {
        BIO_free(in);
    }
    if (NULL != out) {
        BIO_free(out);
    }
    free(buf);
    return rc;
}

static pmix_status_t load_trust(void)
{
    X509_LOOKUP *lookup;

    trust = X509_STORE_new();
    if (NULL == trust) {
        return PMIX_ERR_NOMEM;
    }
    lookup = X509_STORE_add_lookup(trust, X509_LOOKUP_file());
    if (NULL == lookup ||
        0 >= X509_load_cert_crl_file(lookup, pmix_psec_ssl_params.ca_file, X509_FILETYPE_PEM)) {
        drain_errors("loading the CA file");
        pmix_show_help("help-psec-ssl.txt", "bad-ca", true, pmix_psec_ssl_params.ca_file);
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL != pmix_psec_ssl_params.crl_file) {
        if (0 >= X509_load_crl_file(lookup, pmix_psec_ssl_params.crl_file, X509_FILETYPE_PEM)) {
            drain_errors("loading the CRL file");
            pmix_show_help("help-psec-ssl.txt", "bad-ca", true, pmix_psec_ssl_params.crl_file);
            return PMIX_ERR_BAD_PARAM;
        }
        X509_STORE_set_flags(trust, X509_V_FLAG_CRL_CHECK);
    }
    return PMIX_SUCCESS;
}

static void release_all(void)
{
    if (NULL != mykey) {
        EVP_PKEY_free(mykey);
        mykey = NULL;
    }
    free(mychain);
    mychain = NULL;
    mychainlen = 0;
    if (NULL != trust) {
        X509_STORE_free(trust);
        trust = NULL;
    }
    pmix_mutex_lock(&seenlock);
    if (seen_ready) {
        PMIX_LIST_DESTRUCT(&seen);
        seen_ready = false;
    }
    pmix_mutex_unlock(&seenlock);
}

static pmix_status_t ssl_init(void)
{
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: ssl init");

    if (0 >= pmix_psec_ssl_params.max_skew) {
        pmix_show_help("help-psec-ssl.txt", "bad-skew", true, pmix_psec_ssl_params.max_skew);
        return PMIX_ERR_BAD_PARAM;
    }
    if ((NULL == pmix_psec_ssl_params.cert_file) != (NULL == pmix_psec_ssl_params.key_file)) {
        pmix_show_help("help-psec-ssl.txt", "half-identity", true);
        return PMIX_ERR_BAD_PARAM;
    }

    /* a select that fails leaves us off the actives list, so finalize is
     * never called for us - clean up here on every failure */
    if (NULL != pmix_psec_ssl_params.cert_file) {
        rc = load_identity();
        if (PMIX_SUCCESS != rc) {
            release_all();
            return rc;
        }
    }
    if (NULL != pmix_psec_ssl_params.ca_file) {
        rc = load_trust();
        if (PMIX_SUCCESS != rc) {
            release_all();
            return rc;
        }
    }
    if (NULL == mykey && NULL == trust) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_mutex_lock(&seenlock);
    PMIX_CONSTRUCT(&seen, pmix_list_t);
    seen_ready = true;
    pmix_mutex_unlock(&seenlock);
    return PMIX_SUCCESS;
}

static void ssl_finalize(void)
{
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: ssl finalize");
    release_all();
}

/* ------------------------------------------------------------------ */

static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred)
{
    unsigned char *buf = NULL;
    size_t signedlen, siglen = 0;
    EVP_MD_CTX *md = NULL;
    pmix_status_t rc = PMIX_ERROR;

    PMIX_HIDE_UNUSED_PARAMS(peer);

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: ssl create_cred");

    PMIX_BYTE_OBJECT_CONSTRUCT(cred);

    if (!pmix_psec_base_check_directives("ssl", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    if (NULL == mykey) {
        /* we can check other people's credentials but have none of our
         * own - no cert_file/key_file was given */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    signedlen = SSL_HDR_LEN + mychainlen;
    buf = (unsigned char *) malloc(signedlen + 4 + SSL_MAX_SIG);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(buf, SSL_MAGIC, SSL_MAGIC_LEN);
    put_u32(buf + SSL_OFF_UID, (uint32_t) geteuid());
    put_u32(buf + SSL_OFF_GID, (uint32_t) getegid());
    put_u64(buf + SSL_OFF_TIME, (uint64_t) time(NULL));
    if (1 != RAND_bytes(buf + SSL_OFF_NONCE, SSL_NONCE_LEN)) {
        drain_errors("generating a nonce");
        goto done;
    }
    put_u32(buf + SSL_OFF_CHAIN, (uint32_t) mychainlen);
    memcpy(buf + SSL_HDR_LEN, mychain, mychainlen);

    md = EVP_MD_CTX_new();
    if (NULL == md ||
        1 != EVP_DigestSignInit(md, NULL, digest_for(mykey), NULL, mykey) ||
        1 != EVP_DigestSign(md, NULL, &siglen, buf, signedlen) ||
        SSL_MAX_SIG < siglen ||
        1 != EVP_DigestSign(md, buf + signedlen + 4, &siglen, buf, signedlen)) {
        drain_errors("signing the credential");
        goto done;
    }
    put_u32(buf + signedlen, (uint32_t) siglen);

    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 1);
        if (NULL == *info) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        *ninfo = 1;
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "ssl", PMIX_STRING);
    }
    cred->bytes = (char *) buf;
    cred->size = signedlen + 4 + siglen;
    buf = NULL;
    rc = PMIX_SUCCESS;

done:
    if (NULL != md) {
        EVP_MD_CTX_free(md);
    }
    free(buf);
    return rc;
}

/* The certificate names its user in its subject's common name, and that
 * name must be a local account: its uid is the identity the credential
 * is allowed to claim. Exactly one CN, printable, of a sane length -
 * anything else is refused rather than guessed at. */
static bool cert_uid(X509 *leaf, uid_t *uid)
{
    X509_NAME *name;
    X509_NAME_ENTRY *entry;
    ASN1_STRING *data;
    const unsigned char *bytes;
    char user[256], *pwbuf;
    struct passwd pwd, *pw = NULL;
    long sz;
    int idx, len;

    name = X509_get_subject_name(leaf);
    if (NULL == name) {
        return false;
    }
    idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (0 > idx || 0 <= X509_NAME_get_index_by_NID(name, NID_commonName, idx)) {
        return false; /* none, or more than one to choose between */
    }
    entry = X509_NAME_get_entry(name, idx);
    data = (NULL == entry) ? NULL : X509_NAME_ENTRY_get_data(entry);
    if (NULL == data) {
        return false;
    }
    len = ASN1_STRING_length(data);
    bytes = ASN1_STRING_get0_data(data);
    if (NULL == bytes || 0 >= len || (int) sizeof(user) <= len ||
        NULL != memchr(bytes, '\0', (size_t) len)) {
        return false;
    }
    memcpy(user, bytes, (size_t) len);
    user[len] = '\0';

    sz = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (0 >= sz) {
        sz = 16384;
    }
    pwbuf = (char *) malloc((size_t) sz);
    if (NULL == pwbuf) {
        return false;
    }
    if (0 != getpwnam_r(user, &pwd, pwbuf, (size_t) sz, &pw) || NULL == pw) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec:ssl: certificate names \"%s\", who is not a user here", user);
        free(pwbuf);
        return false;
    }
    *uid = pw->pw_uid;
    free(pwbuf);
    return true;
}

/* true if this nonce is new - and then remember it. Called last, once the
 * credential has passed every other check, so only valid credentials are
 * recorded. Entries are kept for the whole window in which a credential
 * is accepted. */
static bool first_use(const unsigned char *nonce, int64_t when, int64_t now)
{
    pmix_psec_ssl_seen_t *s, *next;
    int64_t window = 2 * (int64_t) pmix_psec_ssl_params.max_skew;
    bool fresh = true;

    pmix_mutex_lock(&seenlock);
    if (!seen_ready) {
        pmix_mutex_unlock(&seenlock);
        return false;
    }
    PMIX_LIST_FOREACH_SAFE (s, next, &seen, pmix_psec_ssl_seen_t) {
        if (s->when < now - window) {
            pmix_list_remove_item(&seen, &s->super);
            PMIX_RELEASE(s);
            continue;
        }
        if (0 == memcmp(s->nonce, nonce, SSL_NONCE_LEN)) {
            fresh = false;
        }
    }
    if (fresh) {
        s = PMIX_NEW(pmix_psec_ssl_seen_t);
        if (NULL == s) {
            fresh = false; /* cannot remember it, so cannot accept it */
        } else {
            memcpy(s->nonce, nonce, SSL_NONCE_LEN);
            s->when = when;
            pmix_list_append(&seen, &s->super);
        }
    }
    pmix_mutex_unlock(&seenlock);
    return fresh;
}

#define REFUSE(why)                                                            \
    do {                                                                       \
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,      \
                            "psec:ssl: credential refused: %s", (why));        \
        rc = PMIX_ERR_INVALID_CRED;                                            \
        goto done;                                                             \
    } while (0)

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    const unsigned char *p;
    uint32_t chainlen, siglen, u32;
    size_t signedlen;
    uid_t uid, certuid;
    gid_t gid;
    int64_t when, now, skew;
    BIO *in = NULL;
    X509 *cert, *leaf = NULL;
    STACK_OF(X509) *chain = NULL;
    X509_STORE_CTX *ctx = NULL;
    EVP_MD_CTX *md = NULL;
    EVP_PKEY *pub;
    int ncerts = 0;
    pmix_status_t rc = PMIX_ERR_INVALID_CRED;

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: ssl validate_cred %s", (NULL == cred) ? "NULL" : "NON-NULL");

    if (!pmix_psec_base_check_directives("ssl", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    if (NULL == trust) {
        /* we can make credentials but were given no CA to check anyone
         * else's against */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* the framing - every length came from the peer */
    if (NULL == cred || NULL == cred->bytes || SSL_HDR_LEN + 4 > cred->size) {
        return PMIX_ERR_INVALID_CRED;
    }
    p = (const unsigned char *) cred->bytes;
    if (0 != memcmp(p, SSL_MAGIC, SSL_MAGIC_LEN)) {
        return PMIX_ERR_INVALID_CRED;
    }
    chainlen = get_u32(p + SSL_OFF_CHAIN);
    if (0 == chainlen || SSL_MAX_CHAIN < chainlen ||
        cred->size - SSL_HDR_LEN - 4 < chainlen) {
        return PMIX_ERR_INVALID_CRED;
    }
    signedlen = SSL_HDR_LEN + chainlen;
    siglen = get_u32(p + signedlen);
    if (0 == siglen || SSL_MAX_SIG < siglen || signedlen + 4 + siglen != cred->size) {
        return PMIX_ERR_INVALID_CRED;
    }
    uid = (uid_t) get_u32(p + SSL_OFF_UID);
    gid = (gid_t) get_u32(p + SSL_OFF_GID);
    when = (int64_t) get_u64(p + SSL_OFF_TIME);

    /* the chain: the first certificate is the peer's, the rest are
     * intermediates it offers - untrusted until they lead to a CA of ours */
    in = BIO_new_mem_buf(p + SSL_HDR_LEN, (int) chainlen);
    chain = sk_X509_new_null();
    if (NULL == in || NULL == chain) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    while (NULL != (cert = PEM_read_bio_X509(in, NULL, no_passphrase, NULL))) {
        if (SSL_MAX_CERTS <= ncerts++) {
            X509_free(cert);
            REFUSE("too many certificates");
        }
        if (NULL == leaf) {
            leaf = cert;
        } else if (0 >= sk_X509_push(chain, cert)) {
            X509_free(cert);
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
    }
    ERR_clear_error();
    if (NULL == leaf) {
        REFUSE("no certificate");
    }

    ctx = X509_STORE_CTX_new();
    if (NULL == ctx || 1 != X509_STORE_CTX_init(ctx, trust, leaf, chain)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    /* a certificate issued for authenticating clients: if it restricts
     * its key's use, it has to allow this one */
    X509_STORE_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT);
    if (1 != X509_verify_cert(ctx)) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec:ssl: certificate refused: %s",
                            X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
        ERR_clear_error();
        rc = PMIX_ERR_INVALID_CRED;
        goto done;
    }

    /* the statement must have been signed by that certificate's key */
    pub = X509_get0_pubkey(leaf);
    md = EVP_MD_CTX_new();
    if (NULL == pub || NULL == md ||
        1 != EVP_DigestVerifyInit(md, NULL, digest_for(pub), NULL, pub) ||
        1 != EVP_DigestVerify(md, p + signedlen + 4, siglen, p, signedlen)) {
        drain_errors("checking the signature");
        REFUSE("the signature is not the certificate's");
    }

    /* and it may claim only the user the certificate names */
    if (!cert_uid(leaf, &certuid)) {
        REFUSE("the certificate does not name a user of this host");
    }
    if (certuid != uid) {
        REFUSE("the credential claims a uid other than the certificate's user");
    }
    if (!pmix_psec_base_gid_held(uid, gid)) {
        REFUSE("the credential claims a group its user does not hold");
    }

    /* recent enough to still be accepted, and not already used */
    now = (int64_t) time(NULL);
    skew = pmix_psec_ssl_params.max_skew;
    if (now - when > skew || when - now > skew) {
        REFUSE("the credential is outside the accepted time window");
    }

    /* the same comparison every mechanism makes with what the peer was
     * registered as - or, for a tool, what it said in its handshake */
    if (NULL != pr->info && (uid != pr->info->uid || gid != pr->info->gid)) {
        REFUSE("the credential's identity is not the one this peer connected as");
    }

    if (!first_use(p + SSL_OFF_NONCE, when, now)) {
        REFUSE("the credential has been used before");
    }

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec:ssl: credential valid for uid %lu", (unsigned long) uid);
    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 3);
        if (NULL == *info) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        *ninfo = 3;
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "ssl", PMIX_STRING);
        u32 = uid;
        PMIX_INFO_LOAD(&(*info)[1], PMIX_USERID, &u32, PMIX_UINT32);
        u32 = gid;
        PMIX_INFO_LOAD(&(*info)[2], PMIX_GRPID, &u32, PMIX_UINT32);
    }
    rc = PMIX_SUCCESS;

done:
    if (NULL != md) {
        EVP_MD_CTX_free(md);
    }
    if (NULL != ctx) {
        X509_STORE_CTX_free(ctx);
    }
    if (NULL != leaf) {
        X509_free(leaf);
    }
    if (NULL != chain) {
        sk_X509_pop_free(chain, X509_free);
    }
    if (NULL != in) {
        BIO_free(in);
    }
    return rc;
}
