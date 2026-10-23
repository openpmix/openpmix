/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the psec/ssl component (src/mca/psec/ssl).
 *
 * ssl authenticates a peer on another host with an X.509 certificate:
 * the credential is a signed statement of the peer's uid and gid, a
 * timestamp and a nonce, carrying the certificate chain that signed it.
 * This program mints its own PKI and builds credentials that each differ
 * from a valid one in exactly one respect:
 *
 *   - a certificate from a CA the server does not trust
 *   - an expired certificate, and one whose key usage is for servers
 *   - a certificate naming a user this host does not have
 *   - a claim of a uid other than the certificate's user, or of a group
 *     that user does not hold
 *   - a statement that was altered after it was signed
 *   - a timestamp outside the window, in either direction
 *   - a credential presented twice
 *   - every truncation of a good credential, which must be refused
 *   - a revoked certificate, once a CRL is configured
 *
 * A credential made for one connection between hosts (create_bound,
 * validate_bound) is checked the same way, and must also carry the bytes
 * it was bound to and say whose it is: one bound to other bytes, made as
 * a tool and presented as a server's or the reverse, presented twice, or
 * presented where the other format is expected is refused. A server's
 * certificate must allow server authentication. Only an end with both a
 * CA and an identity of its own can take part.
 *
 * The peer stands for a connection, so it holds a descriptor. A
 * credential handed to PMIx_Validate_credential has none, and is
 * checked on its own and reported as the identity it carries - that
 * case is covered separately, with the descriptor taken away.
 *
 * Each of those is built by hand in the documented wire format rather
 * than by create_cred, so the test also pins the format: a module that
 * changed it would refuse every hand-built credential here, the good
 * ones included. The module's own create_cred output is round-tripped
 * too, with an EC key and then an Ed25519 one (which signs without a
 * separate digest, and so takes a different path through the code).
 *
 * The component reads its configuration when it registers, so each
 * configuration is a fresh open of the psec framework with the
 * PMIX_MCA_psec_ssl_* variables set beforehand.
 *
 * Built only when psec/ssl was built against a real OpenSSL.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/psec/base/base.h"
#include "src/runtime/pmix_init_util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_GRP_H
#    include <grp.h>
#endif

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "ssl_test_pki.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* ---------------------------------------------------------------- *
 * a small PKI
 * ---------------------------------------------------------------- */

static X509_CRL *make_crl(X509 *ca, EVP_PKEY *cakey, long revoked_serial)
{
    X509_CRL *crl = X509_CRL_new();
    X509_REVOKED *r = X509_REVOKED_new();
    ASN1_TIME *t = ASN1_TIME_new();
    ASN1_INTEGER *s = ASN1_INTEGER_new();

    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(ca));
    X509_gmtime_adj(t, -60);
    X509_CRL_set1_lastUpdate(crl, t);
    X509_gmtime_adj(t, 86400);
    X509_CRL_set1_nextUpdate(crl, t);
    ASN1_INTEGER_set(s, revoked_serial);
    X509_REVOKED_set_serialNumber(r, s);
    X509_gmtime_adj(t, -60);
    X509_REVOKED_set_revocationDate(r, t);
    X509_CRL_add0_revoked(crl, r);
    X509_CRL_sort(crl);
    X509_CRL_sign(crl, cakey, EVP_sha256());
    ASN1_TIME_free(t);
    ASN1_INTEGER_free(s);
    return crl;
}

static void write_crl(const char *path, X509_CRL *crl)
{
    FILE *fp = fopen(path, "w");

    PEM_write_X509_CRL(fp, crl);
    fclose(fp);
}

/* ---------------------------------------------------------------- *
 * the credential, built by hand in the documented format
 * ---------------------------------------------------------------- */

static void put32(unsigned char *p, uint32_t v)
{
    v = htonl(v);
    memcpy(p, &v, 4);
}

#define HDR (8 + 4 + 4 + 8 + 16 + 4)

static void build_cred(pmix_byte_object_t *cred, EVP_PKEY *key, const char *chain, size_t chainlen,
                  uint32_t uid, uint32_t gid, int64_t when)
{
    size_t signedlen = HDR + chainlen, siglen = 0;
    unsigned char *buf = (unsigned char *) malloc(signedlen + 4 + 2048);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    uint64_t w = (uint64_t) when;

    memcpy(buf, "PMIXSSL1", 8);
    put32(buf + 8, uid);
    put32(buf + 12, gid);
    put32(buf + 16, (uint32_t) (w >> 32));
    put32(buf + 20, (uint32_t) (w & 0xffffffffULL));
    RAND_bytes(buf + 24, 16);
    put32(buf + 40, (uint32_t) chainlen);
    memcpy(buf + HDR, chain, chainlen);
    EVP_DigestSignInit(md, NULL, md_for(key), NULL, key);
    EVP_DigestSign(md, NULL, &siglen, buf, signedlen);
    EVP_DigestSign(md, buf + signedlen + 4, &siglen, buf, signedlen);
    EVP_MD_CTX_free(md);
    put32(buf + signedlen, (uint32_t) siglen);
    cred->bytes = (char *) buf;
    cred->size = signedlen + 4 + siglen;
}

/* the same statement in the bound format, PMIXSSL2: role and the bytes
 * it is bound to come after the nonce. bindlen is what is written in the
 * length field, which a bad case makes disagree with what follows */
static void build_bound(pmix_byte_object_t *cred, EVP_PKEY *key, const char *chain,
                        size_t chainlen, uint32_t uid, uint32_t gid, int64_t when,
                        uint8_t role, const pmix_byte_object_t *bind, uint32_t bindlen)
{
    size_t off = 8 + 4 + 4 + 8 + 16;
    size_t signedlen = off + 1 + 4 + bind->size + 4 + chainlen, siglen = 0;
    unsigned char *buf = (unsigned char *) malloc(signedlen + 4 + 2048);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    uint64_t w = (uint64_t) when;

    memcpy(buf, "PMIXSSL2", 8);
    put32(buf + 8, uid);
    put32(buf + 12, gid);
    put32(buf + 16, (uint32_t) (w >> 32));
    put32(buf + 20, (uint32_t) (w & 0xffffffffULL));
    RAND_bytes(buf + 24, 16);
    buf[off] = role;
    put32(buf + off + 1, bindlen);
    memcpy(buf + off + 5, bind->bytes, bind->size);
    off += 5 + bind->size;
    put32(buf + off, (uint32_t) chainlen);
    memcpy(buf + off + 4, chain, chainlen);
    EVP_DigestSignInit(md, NULL, md_for(key), NULL, key);
    EVP_DigestSign(md, NULL, &siglen, buf, signedlen);
    EVP_DigestSign(md, buf + signedlen + 4, &siglen, buf, signedlen);
    EVP_MD_CTX_free(md);
    put32(buf + signedlen, (uint32_t) siglen);
    cred->bytes = (char *) buf;
    cred->size = signedlen + 4 + siglen;
}

/* ---------------------------------------------------------------- */

static pmix_peer_t *peer;

static pmix_status_t check(pmix_psec_module_t *mod, const pmix_byte_object_t *cred)
{
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, cred);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    return rc;
}

/* whether ssl is on the list of mechanisms we have, or on the list we
 * tell peers we accept */
static bool lists_ssl(bool accepted)
{
    char *list, **names;
    bool found = false;
    int n;

    list = accepted ? pmix_psec_base_get_accepted_modules()
                    : pmix_psec_base_get_available_modules();
    if (NULL == list) {
        return false;
    }
    names = PMIx_Argv_split(list, ',');
    for (n = 0; NULL != names && NULL != names[n]; n++) {
        if (0 == strcmp(names[n], "ssl")) {
            found = true;
        }
    }
    PMIx_Argv_free(names);
    free(list);
    return found;
}

/* whether native comes ahead of ssl in the priority order. Not whether
 * native is the default: with munged running, munge outranks both */
static bool native_before_ssl(void)
{
    pmix_psec_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_psec_globals.actives, pmix_psec_base_active_module_t) {
        if (0 == strcmp(active->module->name, "native")) {
            return true;
        }
        if (0 == strcmp(active->module->name, "ssl")) {
            return false;
        }
    }
    return false;
}

/* the scratch PKI, keys included, on a failure that ends the test early:
 * every file in the directory the test made, then the directory */
static void remove_pki(const char *dir)
{
    char path[1200];
    struct dirent *ent;
    DIR *d;

    d = opendir(dir);
    if (NULL != d) {
        while (NULL != (ent = readdir(d))) {
            if ('.' == ent->d_name[0]) {
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            (void) unlink(path);
        }
        closedir(d);
    }
    (void) rmdir(dir);
}

static bool open_psec(void)
{
    if (PMIX_SUCCESS != pmix_mca_base_framework_open(&pmix_psec_base_framework, 0) ||
        PMIX_SUCCESS != pmix_psec_base_select()) {
        return false;
    }
    return true;
}

static void close_psec(void)
{
    (void) pmix_mca_base_framework_close(&pmix_psec_base_framework);
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/pmix_psec_ssl.XXXXXX";
    char path[1024], cafile[1024], crlfile[1024], certfile[1024], keyfile[1024];
    char edcert[1024], edkey[1024], openkey[1024], bothcert[1024];
    EVP_PKEY *cakey, *intkey, *key, *otherkey, *edk, *rootca2key;
    X509 *ca, *intca, *leaf, *leaf_int, *leaf_other, *leaf_expired, *leaf_server,
        *leaf_nouser, *leaf_revoked, *leaf_ed, *leaf_both;
    pmix_byte_object_t bind, other;
    uid_t buid;
    gid_t bgid;
    X509_CRL *crl;
    char *pem, *c_leaf, *c_int, *c_other, *c_expired, *c_server, *c_nouser, *c_revoked, *c_both;
    size_t n, l_leaf, l_int, l_other, l_expired, l_server, l_nouser, l_revoked, l_both;
    pmix_byte_object_t cred, trunc;
    pmix_info_t *results = NULL, *ptr;
    size_t nresults = 0, i;
    pmix_psec_module_t *mod;
    pmix_status_t rc;
    struct passwd *pw;
    uint32_t me, mygid, other_gid;
    int64_t now = (int64_t) time(NULL);
    bool all_refused;

    (void) argc;
    (void) argv;

    pw = getpwuid(geteuid());
    if (NULL == pw || NULL == mkdtemp(dir)) {
        fprintf(stdout, "no passwd entry or temp dir - skipping\n");
        return 77;
    }
    me = (uint32_t) geteuid();
    mygid = (uint32_t) getegid();

    /* the PKI: a root, an intermediate under it, a second root nobody
     * trusts, and leaves for every case */
    cakey = gen_key(EVP_PKEY_EC);
    intkey = gen_key(EVP_PKEY_EC);
    key = gen_key(EVP_PKEY_EC);
    otherkey = gen_key(EVP_PKEY_EC);
    rootca2key = gen_key(EVP_PKEY_EC);
    edk = gen_key(EVP_PKEY_ED25519);
    ca = make_cert(cakey, "PMIx test root", NULL, NULL, 1, -60, 86400, NULL);
    intca = make_cert(intkey, "PMIx test intermediate", ca, cakey, 2, -60, 86400, NULL);
    leaf = make_cert(key, pw->pw_name, ca, cakey, 10, -60, 86400, "clientAuth");
    leaf_int = make_cert(key, pw->pw_name, intca, intkey, 11, -60, 86400, "clientAuth");
    leaf_other = make_cert(otherkey, pw->pw_name,
                           make_cert(rootca2key, "Nobody's root", NULL, NULL, 1, -60, 86400, NULL),
                           rootca2key, 12, -60, 86400, "clientAuth");
    leaf_expired = make_cert(key, pw->pw_name, ca, cakey, 13, -2 * 86400, -86400, "clientAuth");
    leaf_server = make_cert(key, pw->pw_name, ca, cakey, 14, -60, 86400, "serverAuth");
    leaf_nouser = make_cert(key, "pmix-no-such-user-q7x", ca, cakey, 15, -60, 86400, "clientAuth");
    leaf_revoked = make_cert(key, pw->pw_name, ca, cakey, 99, -60, 86400, "clientAuth");
    leaf_ed = make_cert(edk, pw->pw_name, ca, cakey, 16, -60, 86400, "clientAuth");
    leaf_both = make_cert(key, pw->pw_name, ca, cakey, 17, -60, 86400, "clientAuth,serverAuth");
    crl = make_crl(ca, cakey, 99);
    c_leaf = chain_pem(leaf, NULL, &l_leaf);
    c_int = chain_pem(leaf_int, intca, &l_int);
    c_other = chain_pem(leaf_other, NULL, &l_other);
    c_expired = chain_pem(leaf_expired, NULL, &l_expired);
    c_server = chain_pem(leaf_server, NULL, &l_server);
    c_nouser = chain_pem(leaf_nouser, NULL, &l_nouser);
    c_revoked = chain_pem(leaf_revoked, NULL, &l_revoked);

    snprintf(cafile, sizeof(cafile), "%s/ca.pem", dir);
    pem = chain_pem(ca, NULL, &n);
    write_file(cafile, pem, n, 0644);
    free(pem);
    snprintf(certfile, sizeof(certfile), "%s/me.pem", dir);
    write_file(certfile, c_leaf, l_leaf, 0644);
    snprintf(keyfile, sizeof(keyfile), "%s/me.key", dir);
    write_key(keyfile, key, 0600);
    snprintf(edcert, sizeof(edcert), "%s/ed.pem", dir);
    pem = chain_pem(leaf_ed, NULL, &n);
    write_file(edcert, pem, n, 0644);
    free(pem);
    snprintf(edkey, sizeof(edkey), "%s/ed.key", dir);
    write_key(edkey, edk, 0600);
    snprintf(bothcert, sizeof(bothcert), "%s/both.pem", dir);
    c_both = chain_pem(leaf_both, NULL, &l_both);
    write_file(bothcert, c_both, l_both, 0644);
    snprintf(openkey, sizeof(openkey), "%s/open.key", dir);
    write_key(openkey, key, 0644);
    snprintf(crlfile, sizeof(crlfile), "%s/crl.pem", dir);
    write_crl(crlfile, crl);

    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        remove_pki(dir);
        return 1;
    }

    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = me;
    peer->info->gid = mygid;
    /* ssl reads nothing from the connection, only that there is one */
    peer->sd = open("/dev/null", O_RDONLY);
    if (0 > peer->sd) {
        fprintf(stdout, "cannot open /dev/null - skipping\n");
        return 77;
    }

    /* ---- nothing configured: the component stays out of the way ---- */
    fprintf(stdout, "\n=== unconfigured ===\n");
    if (!open_psec()) {
        report("psec opens with nothing configured", 0);
        remove_pki(dir);
        return 1;
    }
    report("ssl is not active when nothing is configured",
           NULL == pmix_psec_base_assign_module("ssl"));
    close_psec();

    /* ---- a CA to trust, and an identity of our own ---- */
    fprintf(stdout, "\n=== CA + EC identity ===\n");
    setenv("PMIX_MCA_psec_ssl_ca_file", cafile, 1);
    setenv("PMIX_MCA_psec_ssl_cert_file", certfile, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", keyfile, 1);
    setenv("PMIX_MCA_psec_ssl_max_skew", "30", 1);
    if (!open_psec() || NULL == (mod = pmix_psec_base_assign_module("ssl"))) {
        report("ssl is active once configured", 0);
        remove_pki(dir);
        return 1;
    }
    report("ssl is active once configured", 1);
    report("native still outranks ssl by default", native_before_ssl());
    report("with a CA, ssl is among the mechanisms offered to peers", lists_ssl(true));

    /* our own credential, round trip */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("create_cred succeeds", PMIX_SUCCESS == rc && 0 < cred.size);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred accepts it", PMIX_SUCCESS == rc);
    ptr = NULL;
    for (i = 0; i < nresults; i++) {
        if (PMIx_Check_key(results[i].key, PMIX_USERID)) {
            ptr = &results[i];
        }
    }
    report("validate_cred returns the uid",
           NULL != ptr && PMIX_UINT32 == ptr->value.type && me == ptr->value.data.uint32);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    report("the same credential is refused a second time",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));

    /* every truncation of a good credential: refused, and survived */
    all_refused = true;
    for (n = 0; n < cred.size; n++) {
        trunc.bytes = (char *) malloc(n + 1);
        memcpy(trunc.bytes, cred.bytes, n);
        trunc.size = n;
        if (PMIX_SUCCESS == check(mod, &trunc)) {
            all_refused = false;
        }
        free(trunc.bytes);
    }
    report("every truncation of a credential is refused", all_refused);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* hand-built: the good cases first, so a refusal below is about the
     * one thing each gets wrong and not about the format */
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
    report("a hand-built credential in the documented format is accepted",
           PMIX_SUCCESS == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_int, l_int, me, mygid, now);
    report("a chain through an intermediate is accepted", PMIX_SUCCESS == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* altered after signing */
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
    cred.bytes[8] ^= 0x01; /* the uid */
    report("a credential altered after signing is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
    cred.bytes[cred.size - 1] ^= 0x01; /* the signature */
    report("a credential with a damaged signature is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* signed by a key that is not the certificate's */
    build_cred(&cred, otherkey, c_leaf, l_leaf, me, mygid, now);
    report("a credential signed by another key is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* certificates that must not be trusted */
    build_cred(&cred, otherkey, c_other, l_other, me, mygid, now);
    report("a certificate from an untrusted CA is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_expired, l_expired, me, mygid, now);
    report("an expired certificate is refused", PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_server, l_server, me, mygid, now);
    report("a certificate for server use only is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_nouser, l_nouser, me, mygid, now);
    report("a certificate naming no user of this host is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* claims the certificate does not support */
    build_cred(&cred, key, c_leaf, l_leaf, (0 == me) ? 4242 : 0, mygid, now);
    peer->info->uid = (0 == me) ? 4242 : 0;
    report("a claim of another uid is refused, even matching the registration",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    peer->info->uid = me;
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    if (0 != me) {
        other_gid = 54321;
#ifdef HAVE_GRP_H
        while (NULL != getgrgid((gid_t) other_gid) || mygid == other_gid) {
            other_gid++;
        }
#endif
        build_cred(&cred, key, c_leaf, l_leaf, me, other_gid, now);
        peer->info->gid = other_gid;
        report("a claim of a group the user does not hold is refused",
               PMIX_ERR_INVALID_CRED == check(mod, &cred));
        peer->info->gid = mygid;
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    }
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
    peer->info->uid = me + 1;
    report("a credential for someone other than the peer is refused",
           PMIX_ERR_INVALID_CRED == check(mod, &cred));
    peer->info->uid = me;
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    {
        /* with no connection - PMIx_Validate_credential, whose peer is
         * the validator - the credential is checked on its own, and its
         * own identity reported. The validator's is not the holder's */
        int sd = peer->sd;
        pmix_rank_info_t *info = peer->info;
        uint32_t u32 = (uint32_t) -1;

        peer->sd = -1;
        build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
        peer->info->uid = me + 1;
        peer->info->gid = mygid + 1;
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults,
                                &cred);
        for (i = 0; i < nresults; i++) {
            if (PMIX_CHECK_KEY(&results[i], PMIX_USERID) &&
                PMIX_UINT32 == results[i].value.type) {
                u32 = results[i].value.data.uint32;
            }
        }
        report("with no connection, a credential is not held to the validator's identity",
               PMIX_SUCCESS == rc);
        report("and is reported as the identity it carries", me == u32);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
            results = NULL;
            nresults = 0;
        }
        peer->info->uid = me;
        peer->info->gid = mygid;
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);

        peer->info = NULL;
        build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
        report("with no connection and no peer identity, a credential is accepted",
               PMIX_SUCCESS == check(mod, &cred));
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);

        peer->sd = sd;
        build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
        report("a connection with no identity to check against is refused",
               PMIX_ERR_INVALID_CRED == check(mod, &cred));
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        peer->info = info;
    }

    /* the time window - max_skew is 30 */
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now - 120);
    report("a stale credential is refused", PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now + 120);
    report("a credential from the future is refused", PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now - 20);
    report("a credential inside the window is accepted", PMIX_SUCCESS == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* not ours to answer */
    {
        pmix_info_t directive;

        build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
        PMIX_INFO_LOAD(&directive, PMIX_CRED_TYPE, "munge", PMIX_STRING);
        rc = mod->validate_cred((struct pmix_peer_t *) peer, &directive, 1, NULL, NULL, &cred);
        report("validate_cred declines another mechanism", PMIX_ERR_NOT_SUPPORTED == rc);
        PMIX_INFO_DESTRUCT(&directive);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    }

    /* a certificate for clients only proves a tool, never a server */
    bind.bytes = (char *) "pmix-ra1Sone-connection's-two-nonces-xx";
    bind.size = strlen(bind.bytes);
    report("with a CA and an identity, ssl can authenticate across hosts",
           pmix_psec_base_remote_capable(mod));
    rc = mod->create_bound(&bind, false, (uid_t) -1, &cred);
    report("a bound credential is made as a tool",
           PMIX_SUCCESS == rc && PMIX_SUCCESS == mod->validate_bound(&bind, false, &cred, &buid,
                                                                     &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    rc = mod->create_bound(&bind, true, (uid_t) -1, &cred);
    report("a certificate for client use only does not prove a server",
           PMIX_SUCCESS == rc &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, true, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    close_psec();

    /* ---- credentials for one connection between hosts ---- */
    fprintf(stdout, "\n=== bound credentials ===\n");
    setenv("PMIX_MCA_psec_ssl_cert_file", bothcert, 1);
    if (!open_psec() || NULL == (mod = pmix_psec_base_assign_module("ssl"))) {
        report("ssl is active with a client and server certificate", 0);
        remove_pki(dir);
        return 1;
    }
    other.bytes = (char *) "pmix-ra1Sone-connection's-two-nonces-xy";
    other.size = strlen(other.bytes);

    rc = mod->create_bound(&bind, true, (uid_t) -1, &cred);
    buid = (uid_t) -1;
    bgid = (gid_t) -1;
    report("a server's bound credential is accepted as a server's",
           PMIX_SUCCESS == rc &&
               PMIX_SUCCESS == mod->validate_bound(&bind, true, &cred, &buid, &bgid));
    report("and gives the identity it proves", me == (uint32_t) buid && mygid == (uint32_t) bgid);
    report("the same credential is refused a second time",
           PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, true, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    rc = mod->create_bound(&bind, true, (uid_t) -1, &cred);
    report("a server's bound credential is refused as a tool's",
           PMIX_SUCCESS == rc &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    rc = mod->create_bound(&bind, false, (uid_t) -1, &cred);
    report("a tool's bound credential is refused as a server's",
           PMIX_SUCCESS == rc &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, true, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    rc = mod->create_bound(&bind, false, (uid_t) -1, &cred);
    report("a credential bound to other bytes is refused",
           PMIX_SUCCESS == rc &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&other, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    rc = mod->create_bound(&bind, false, (uid_t) -1, &cred);
    report("a bound credential is refused by validate_cred",
           PMIX_SUCCESS == rc && PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, NULL, NULL, &cred);
    report("a credential for PMIx_Validate_credential is refused as a bound one",
           PMIX_SUCCESS == rc &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    /* the documented layout, built by hand: a change to the format fails
     * the good case as well as the bad ones */
    build_bound(&cred, key, c_both, l_both, me, mygid, now, 0, &bind, (uint32_t) bind.size);
    report("a hand-built bound credential in the documented format is accepted",
           PMIX_SUCCESS == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_bound(&cred, key, c_both, l_both, me, mygid, now, 1, &bind, (uint32_t) bind.size);
    report("...and as a server's, with role 1",
           PMIX_SUCCESS == mod->validate_bound(&bind, true, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_bound(&cred, key, c_both, l_both, me, mygid, now, 2, &bind, (uint32_t) bind.size);
    report("a role that is neither is refused",
           PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, true, &cred, &buid, &bgid) &&
               PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_bound(&cred, key, c_both, l_both, me, mygid, now, 0, &bind, (uint32_t) bind.size - 1);
    report("a bind length that is not the bytes' is refused",
           PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_bound(&cred, key, c_both, l_both, me, mygid, now, 0, &bind, 0x7fffffff);
    report("a bind length past the end of the credential is refused",
           PMIX_ERR_INVALID_CRED == mod->validate_bound(&bind, false, &cred, &buid, &bgid));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    rc = mod->create_bound(&bind, false, (uid_t) -1, &cred);
    all_refused = (PMIX_SUCCESS == rc);
    for (n = 0; all_refused && n < cred.size; n++) {
        trunc.bytes = cred.bytes;
        trunc.size = n;
        if (PMIX_ERR_INVALID_CRED != mod->validate_bound(&bind, false, &trunc, &buid, &bgid)) {
            all_refused = false;
        }
    }
    report("every truncation of a bound credential is refused", all_refused);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    close_psec();

    /* ---- a CA alone: accept, but prove nothing ---- */
    fprintf(stdout, "\n=== CA, no identity ===\n");
    unsetenv("PMIX_MCA_psec_ssl_cert_file");
    unsetenv("PMIX_MCA_psec_ssl_key_file");
    if (!open_psec() || NULL == (mod = pmix_psec_base_assign_module("ssl"))) {
        report("ssl is active with a CA alone", 0);
        remove_pki(dir);
        return 1;
    }
    report("with no identity, ssl cannot authenticate across hosts",
           !pmix_psec_base_remote_capable(mod));
    close_psec();
    setenv("PMIX_MCA_psec_ssl_cert_file", certfile, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", keyfile, 1);

    /* ---- revocation, and an Ed25519 identity ---- */
    fprintf(stdout, "\n=== CRL + Ed25519 identity ===\n");
    setenv("PMIX_MCA_psec_ssl_crl_file", crlfile, 1);
    setenv("PMIX_MCA_psec_ssl_cert_file", edcert, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", edkey, 1);
    if (!open_psec() || NULL == (mod = pmix_psec_base_assign_module("ssl"))) {
        report("ssl is active with a CRL", 0);
        remove_pki(dir);
        return 1;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, NULL, NULL, &cred);
    report("an Ed25519 identity creates a credential", PMIX_SUCCESS == rc);
    report("and it is accepted", PMIX_SUCCESS == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_revoked, l_revoked, me, mygid, now);
    report("a revoked certificate is refused", PMIX_ERR_INVALID_CRED == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    build_cred(&cred, key, c_leaf, l_leaf, me, mygid, now);
    report("an unrevoked certificate is still accepted", PMIX_SUCCESS == check(mod, &cred));
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    close_psec();
    unsetenv("PMIX_MCA_psec_ssl_crl_file");

    /* ---- an identity and no CA: connect with ssl, but accept nobody ---- */
    fprintf(stdout, "\n=== identity, no CA ===\n");
    unsetenv("PMIX_MCA_psec_ssl_ca_file");
    setenv("PMIX_MCA_psec_ssl_cert_file", certfile, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", keyfile, 1);
    if (!open_psec() || NULL == (mod = pmix_psec_base_assign_module("ssl"))) {
        report("ssl is active with an identity alone", 0);
        remove_pki(dir);
        return 1;
    }
    report("with no CA, ssl is still available to connect with", lists_ssl(false));
    report("but is not offered to peers", !lists_ssl(true));
    report("nor can it authenticate across hosts", !pmix_psec_base_remote_capable(mod));
    close_psec();
    setenv("PMIX_MCA_psec_ssl_ca_file", cafile, 1);

    /* ---- a private key anyone may read is refused ---- */
    fprintf(stdout, "\n=== an exposed private key ===\n");
    setenv("PMIX_MCA_psec_ssl_cert_file", certfile, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", openkey, 1);
    if (!open_psec()) {
        report("psec opens with an exposed key", 0);
        remove_pki(dir);
        return 1;
    }
    report("ssl refuses to start with a world-readable key",
           NULL == pmix_psec_base_assign_module("ssl"));
    close_psec();

    /* ---- a key and certificate that do not belong together ---- */
    snprintf(path, sizeof(path), "%s/other.key", dir);
    write_key(path, otherkey, 0600);
    setenv("PMIX_MCA_psec_ssl_key_file", path, 1);
    if (!open_psec()) {
        report("psec opens with a mismatched key", 0);
        remove_pki(dir);
        return 1;
    }
    report("ssl refuses a key that is not the certificate's",
           NULL == pmix_psec_base_assign_module("ssl"));
    close_psec();

    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer);

    /* tidy the scratch PKI */
    unlink(cafile);
    unlink(certfile);
    unlink(keyfile);
    unlink(edcert);
    unlink(edkey);
    unlink(bothcert);
    unlink(openkey);
    unlink(crlfile);
    unlink(path);
    rmdir(dir);

    free(c_leaf);
    free(c_int);
    free(c_other);
    free(c_expired);
    free(c_server);
    free(c_nouser);
    free(c_revoked);
    free(c_both);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
