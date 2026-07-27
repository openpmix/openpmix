/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_if.c: the pure address/mask parser
 * (pmix_iftupletoaddr), which needs no interface list at all, and the
 * include/exclude matcher (pmix_ifmatches), which is driven here against
 * a synthetic pmix_if_list rather than whatever this host happens to be
 * wired with -- see build_if_list() for why that matters.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif

#include "src/util/pmix_if.h"
#include "src/util/pmix_string_copy.h"

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

/* Expected network values are assembled in host byte order, matching
 * pmix_iftupletoaddr's output. */
#define ADDR(a, b, c, d) \
    ((uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)))

static void check_ok(const char *label, const char *input,
                     uint32_t exp_net, uint32_t exp_mask)
{
    uint32_t net = 0, mask = 0;
    int rc = pmix_iftupletoaddr(input, &net, &mask);
    report(label, PMIX_SUCCESS == rc && net == exp_net && mask == exp_mask);
}

static void check_err(const char *label, const char *input)
{
    uint32_t net = 0, mask = 0;
    int rc = pmix_iftupletoaddr(input, &net, &mask);
    report(label, PMIX_SUCCESS != rc);
}

/*
 * Append one address to the interface list.  Discovery normally does this,
 * but discovery gives us whatever this host is wired with -- and the case
 * that matters is not universally present.  Building the list by hand lets
 * every platform run the same cases.
 */
static void add_if(const char *name, uint16_t kindex, int family, const char *addr)
{
    pmix_pif_t *intf;

    intf = PMIX_NEW(pmix_pif_t);
    pmix_strncpy(intf->if_name, name, PMIX_IF_NAMESIZE);
    intf->if_index = pmix_list_get_size(&pmix_if_list) + 1;
    intf->if_kernel_index = kindex;
    intf->af_family = family;
    intf->if_flags = IFF_UP;
    ((struct sockaddr *) &intf->if_addr)->sa_family = family;
    if (AF_INET == family) {
        inet_pton(AF_INET, addr, &((struct sockaddr_in *) &intf->if_addr)->sin_addr);
    } else {
        inet_pton(AF_INET6, addr, &((struct sockaddr_in6 *) &intf->if_addr)->sin6_addr);
    }
    pmix_list_append(&pmix_if_list, &intf->super);
}

/*
 * A dual-stack interface holds one list entry per address, and both entries
 * carry the same kernel index.  On Linux the IPv6 entry lands first -- always
 * so for loopback, whose ::1 is discovered before 127.0.0.1 -- so that is the
 * order reproduced here.
 */
static void build_if_list(void)
{
    /* the list is statically declared but only made usable by
     * pmix_pif_base_open(), which no unit test runs */
    PMIX_CONSTRUCT(&pmix_if_list, pmix_list_t);
    add_if("lo", 1, AF_INET6, "::1");
    add_if("lo", 1, AF_INET, "127.0.0.1");
    add_if("eth0", 11, AF_INET6, "fe80::1");
    add_if("eth0", 11, AF_INET, "172.17.0.2");
    add_if("eth1", 12, AF_INET, "10.20.30.40");
}

static void teardown_if_list(void)
{
    pmix_pif_t *intf;

    while (NULL != (intf = (pmix_pif_t *) pmix_list_remove_first(&pmix_if_list))) {
        PMIX_RELEASE(intf);
    }
    PMIX_DESTRUCT(&pmix_if_list);
}

static void check_match(const char *label, int kidx, char *net, int expected)
{
    char *nets[2];

    nets[0] = net;
    nets[1] = NULL;
    report(label, expected == pmix_ifmatches(kidx, nets));
}

static void test_ifmatches(void)
{
    char *both[3];

    build_if_list();

    /* an interface named by subnet must match itself.  This is the
     * regression: pmix_ifmatches() used to read the address out of
     * whichever entry shared the kernel index and came first, so for a
     * dual-stack interface it compared the IPv6 entry's flow label
     * against the requested network and reported no match -- silently
     * dropping every interface an if_include named by subnet. */
    check_match("dual-stack v4 subnet matches", 11, "172.17.0.0/16", PMIX_SUCCESS);
    check_match("dual-stack host route matches", 11, "172.17.0.2/32", PMIX_SUCCESS);
    check_match("loopback subnet matches", 1, "127.0.0.0/8", PMIX_SUCCESS);
    check_match("single-stack subnet matches", 12, "10.20.0.0/16", PMIX_SUCCESS);

    /* a subnet the interface is not on must not match */
    check_match("unrelated subnet does not match", 11, "10.20.0.0/16", PMIX_ERR_NOT_FOUND);

    /* named entries resolve through the kernel index, so both entries of a
     * dual-stack interface answer to the one name */
    check_match("name matches", 11, "eth0", PMIX_SUCCESS);
    check_match("other name does not match", 12, "eth0", PMIX_ERR_NOT_FOUND);

    /* a mixed list matches on either kind of entry */
    both[0] = "eth1";
    both[1] = "172.17.0.0/16";
    both[2] = NULL;
    report("mixed list matches by subnet", PMIX_SUCCESS == pmix_ifmatches(11, both));

    /* an unparseable net is reported rather than treated as "no match" */
    check_match("unparseable net reported", 11, "192.168.1.0/256.0.0.0",
                PMIX_ERR_FABRIC_NOT_PARSEABLE);

    /* an unknown kernel index is an error, not a match */
    check_match("unknown kernel index", 99, "172.17.0.0/16", PMIX_ERROR);

    teardown_if_list();
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_iftupletoaddr unit tests ===\n\n");

    /* integer CIDR prefixes */
    check_ok("cidr /24", "192.168.1.0/24", ADDR(192, 168, 1, 0), 0xFFFFFF00);
    check_ok("cidr /16", "10.20.0.0/16", ADDR(10, 20, 0, 0), 0xFFFF0000);
    /* /32 is a valid single-host match (regression: used to be rejected) */
    check_ok("cidr /32 host route", "10.1.2.3/32", ADDR(10, 1, 2, 3), 0xFFFFFFFF);
    /* /0 matches everything (regression: used to be rejected, and the
     * shift-by-32 was undefined behavior) */
    check_ok("cidr /0 match-all", "0.0.0.0/0", ADDR(0, 0, 0, 0), 0x00000000);

    /* dotted-quad netmask */
    check_ok("dotted mask", "192.168.1.0/255.255.255.0",
             ADDR(192, 168, 1, 0), 0xFFFFFF00);

    /* mask inferred from number of dots when no '/' is present */
    check_ok("implicit /16 from dots", "192.168",
             ADDR(192, 168, 0, 0), 0xFFFF0000);

    /* a malformed dotted mask must be reported, not silently swallowed
     * (regression: the error rc was overwritten by the net parse) */
    check_err("malformed dotted mask rejected", "192.168.1.0/256.0.0.0");

    /* an out-of-range prefix length is rejected */
    check_err("prefix > 32 rejected", "10.0.0.0/33");

    /* an octet that would wrap a uint32_t must be rejected, not accepted
     * as its truncated value (regression) */
    check_err("wrapping octet rejected", "4294967296.0.0.0");

    /* out-of-range octet */
    check_err("octet > 255 rejected", "192.168.1.300/24");

    fprintf(stdout, "\n=== pmix_ifmatches unit tests ===\n\n");
    fprintf(stdout, "-- one case drives an invalid netmask;"
                    " the warning it prints is expected --\n");

    test_ifmatches();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
