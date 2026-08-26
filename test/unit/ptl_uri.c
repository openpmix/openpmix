/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the PTL's address and version parsers.
 *
 * Everything tested here consumes a string that PMIx did not
 * necessarily write. A server URI reaches us from a rendezvous file
 * dropped by some other process, from a PMIX_SERVER_URI environment
 * variable, or from an info key handed in by the caller; a version
 * string is read straight off the wire during the connection handshake.
 * None of those sources is trustworthy, and each of these functions used
 * to index past the end of a string that was shorter than it assumed.
 *
 * The parsers are also a wire-format contract in their own right: the
 * "<nspace>.<rank>;tcp4://<addr>:<port>" shape has to keep round-tripping
 * against what pmix_ptl_base_setup_listener publishes, and against the
 * files older releases wrote, or a tool cannot find its server.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
}

/* ---- pmix_ptl_base_parse_uri ------------------------------------- */

static void test_parse_uri(void)
{
    pmix_status_t rc;
    char *nspace = NULL, *suri = NULL;
    pmix_rank_t rank = PMIX_RANK_UNDEF;

    /* the ordinary case a listener publishes */
    rc = pmix_ptl_base_parse_uri("myns.3;tcp4://127.0.0.1:41234", &nspace, &rank, &suri);
    report("parse_uri: well-formed URI accepted", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        report("parse_uri: nspace extracted", NULL != nspace && 0 == strcmp(nspace, "myns"),
               (NULL == nspace) ? "NULL" : nspace);
        report("parse_uri: rank extracted", 3 == rank, "wrong rank");
        report("parse_uri: address extracted",
               NULL != suri && 0 == strcmp(suri, "tcp4://127.0.0.1:41234"),
               (NULL == suri) ? "NULL" : suri);
        free(nspace);
        free(suri);
        nspace = NULL;
        suri = NULL;
    }

    /* an nspace may itself contain dots - the rank separator is the LAST
     * one, which is why the parser searches from the back. A launcher
     * naming its namespace after a hostname produces exactly this */
    rc = pmix_ptl_base_parse_uri("prterun.node1.example.com.0;tcp4://10.0.0.5:1", &nspace, &rank,
                                 &suri);
    report("parse_uri: dotted nspace accepted", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        report("parse_uri: dotted nspace kept whole",
               NULL != nspace && 0 == strcmp(nspace, "prterun.node1.example.com"),
               (NULL == nspace) ? "NULL" : nspace);
        report("parse_uri: rank after dotted nspace", 0 == rank, "wrong rank");
        free(nspace);
        free(suri);
        nspace = NULL;
        suri = NULL;
    }

    /* the caller may not want the address back at all */
    rc = pmix_ptl_base_parse_uri("myns.1;tcp4://127.0.0.1:2", &nspace, &rank, NULL);
    report("parse_uri: optional address argument", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        free(nspace);
        nspace = NULL;
    }

    /* malformed inputs must be rejected, not parsed into garbage */
    rc = pmix_ptl_base_parse_uri("myns.3", &nspace, &rank, &suri);
    report("parse_uri: missing ';' rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_parse_uri("myns;tcp4://127.0.0.1:1", &nspace, &rank, &suri);
    report("parse_uri: missing rank separator rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_parse_uri("a.1;b;c", &nspace, &rank, &suri);
    report("parse_uri: extra ';' rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_parse_uri("", &nspace, &rank, &suri);
    report("parse_uri: empty string rejected", PMIX_SUCCESS != rc, "accepted");
}

/* ---- pmix_ptl_base_setup_connection ------------------------------ */

static void test_setup_connection(void)
{
    pmix_status_t rc;
    struct sockaddr_storage ss;
    struct sockaddr_in *in;
    size_t len = 0;

    rc = pmix_ptl_base_setup_connection("tcp4://127.0.0.1:41234", &ss, &len);
    report("setup_connection: IPv4 URI accepted", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        in = (struct sockaddr_in *) &ss;
        report("setup_connection: family is AF_INET", AF_INET == in->sin_family, "wrong family");
        report("setup_connection: port converted to network order", 41234 == ntohs(in->sin_port),
               "wrong port");
        report("setup_connection: address length reported",
               sizeof(struct sockaddr_in) == len, "wrong length");
    }

    /* An address that inet_addr cannot convert has to be refused: it
     * returns INADDR_NONE, which is also the encoding of 255.255.255.255,
     * so a caller that ignored the status would connect to the broadcast
     * address */
    rc = pmix_ptl_base_setup_connection("tcp4://not-an-address:1234", &ss, &len);
    report("setup_connection: unconvertible address rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_setup_connection("tcp4://127.0.0.1", &ss, &len);
    report("setup_connection: missing port rejected", PMIX_SUCCESS != rc, "accepted");

    /* Everything below is a string shorter than, or not carrying, the
     * seven-character scheme prefix the parser indexes past. Each of
     * these used to be read from beyond its own end */
    rc = pmix_ptl_base_setup_connection("", &ss, &len);
    report("setup_connection: empty URI rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_setup_connection("tcp4", &ss, &len);
    report("setup_connection: bare scheme rejected", PMIX_SUCCESS != rc, "accepted");

    rc = pmix_ptl_base_setup_connection("tcp4://", &ss, &len);
    report("setup_connection: scheme with no address rejected", PMIX_SUCCESS != rc, "accepted");

    /* Check the specific status here, not merely "not success". A URI
     * carrying some other scheme used to fall into the IPv6 branch,
     * which is a catch-all, and be rejected further down for the
     * unrelated reason that it had no ':' left in it. Requiring
     * NOT_SUPPORTED pins that we recognized the scheme as wrong rather
     * than tripping over the address we tried to read out of it */
    rc = pmix_ptl_base_setup_connection("usock://tmp/pmix.sock", &ss, &len);
    report("setup_connection: unknown scheme rejected as unsupported",
           PMIX_ERR_NOT_SUPPORTED == rc, PMIx_Error_string(rc));

    rc = pmix_ptl_base_setup_connection(NULL, &ss, &len);
    report("setup_connection: NULL URI rejected", PMIX_SUCCESS != rc, "accepted");
}

/* ---- pmix_ptl_base_parse_version --------------------------------- */

static void check_version(const char *name, const char *vers, uint8_t emaj, uint8_t emin,
                          uint8_t erel)
{
    uint8_t maj, min, rel;

    pmix_ptl_base_parse_version(vers, &maj, &min, &rel);
    report(name, emaj == maj && emin == min && erel == rel, "wrong triplet");
}

static void test_parse_version(void)
{
    /* the shape every current peer sends */
    check_version("parse_version: full triplet", "5.1.2", 5, 1, 2);
    /* what a release-candidate string looks like - the trailing text is
     * simply not a number, so the component reads as zero */
    check_version("parse_version: triplet with suffix", "7.0.0a1", 7, 0, 0);
    /* Short forms. A peer is under no obligation to send all three
     * components, and the parser used to step over a separator that was
     * not there - reading past the end of the string for each one */
    check_version("parse_version: major and minor only", "4.2", 4, 2, 0);
    check_version("parse_version: major only", "6", 6, 0, 0);
    check_version("parse_version: empty string", "", 0, 0, 0);
    check_version("parse_version: NULL string", NULL, 0, 0, 0);
    /* trailing separator with nothing after it */
    check_version("parse_version: trailing separator", "3.", 3, 0, 0);
    check_version("parse_version: two trailing separators", "3.1.", 3, 1, 0);
    /* not a version at all */
    check_version("parse_version: non-numeric", "unknown", 0, 0, 0);
}

/* ---- pmix_ptl_base_peer_is_earlier ------------------------------- */

static void check_earlier(const char *name, uint8_t pmaj, uint8_t pmin, uint8_t prel, uint8_t maj,
                          uint8_t min, uint8_t rel, bool expected)
{
    pmix_peer_t peer;
    bool answer;

    memset(&peer, 0, sizeof(peer));
    peer.proc_type.major = pmaj;
    peer.proc_type.minor = pmin;
    peer.proc_type.release = prel;

    answer = pmix_ptl_base_peer_is_earlier(&peer, maj, min, rel);
    report(name, expected == answer, expected ? "said not earlier" : "said earlier");
}

static void test_peer_is_earlier(void)
{
    /* This is what gates every backward-compatibility branch in the
     * library, so each side of each comparison matters */
    check_earlier("is_earlier: older major", 4, 2, 0, 5, 0, 0, true);
    check_earlier("is_earlier: newer major", 6, 0, 0, 5, 0, 0, false);
    check_earlier("is_earlier: older minor", 5, 0, 0, 5, 1, 0, true);
    check_earlier("is_earlier: newer minor", 5, 2, 0, 5, 1, 0, false);
    check_earlier("is_earlier: older release", 5, 1, 1, 5, 1, 2, true);
    check_earlier("is_earlier: newer release", 5, 1, 3, 5, 1, 2, false);
    check_earlier("is_earlier: identical", 5, 1, 2, 5, 1, 2, false);

    /* A major version of zero means the peer never told us what it is -
     * typically a process that never called PMIx_Init. We must not treat
     * that as "ancient" and start speaking a legacy protocol at it */
    check_earlier("is_earlier: unknown peer is not earlier", 0, 0, 0, 5, 1, 2, false);

    /* A wildcard on our side means "don't check this component" */
    check_earlier("is_earlier: wildcard major ignored", 4, 9, 0, PMIX_MAJOR_WILDCARD, 5, 0, false);
    check_earlier("is_earlier: wildcard release ignored", 5, 1, 9, 5, 1, PMIX_RELEASE_WILDCARD,
                  false);
    /* A wildcard on the peer's side means we do not know its version,
     * so we must assume the conservative answer */
    check_earlier("is_earlier: wildcard peer minor assumed earlier", 5, PMIX_MINOR_WILDCARD, 0, 5, 1,
                  0, true);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* A port range the parser cannot read expands to nothing, and the
     * ptl frame indexed element 0 of the result to see whether the user
     * had asked for the wildcard - so "-" (and now any non-numeric
     * value) in this parameter dereferenced a NULL and took the process
     * down inside PMIx_server_init. Setting it here costs nothing: the
     * fallback is the ephemeral port, which is what an unset value asks
     * for anyway. Against an unfixed library this binary dies on a
     * signal before printing a line, rather than failing a case. */
    setenv("PMIX_MCA_ptl_base_ipv4_ports", "-", 1);

    /* the parsers emit verbose output through the ptl framework, so the
     * frameworks have to be open - server_init is the cheapest way to
     * get a fully initialized library */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl address and version parser unit tests ===\n\n");

    test_parse_uri();
    test_setup_connection();
    test_parse_version();
    test_peer_is_earlier();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);

    PMIx_server_finalize();

    return (0 == nfail) ? 0 : 1;
}
