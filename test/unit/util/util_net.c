/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the helpers in src/util/pmix_net.c.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif

#include "src/runtime/pmix_init_util.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_net.h"

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

#if defined(HAVE_STRUCT_SOCKADDR_IN)

/* ------------------------------------------------------------------ */
/* pmix_net_prefix2netmask                                             */
/* ------------------------------------------------------------------ */

static void check_netmask(uint32_t prefixlen, uint32_t expected_host)
{
    char label[64];
    uint32_t got = ntohl(pmix_net_prefix2netmask(prefixlen));
    snprintf(label, sizeof(label), "prefix2netmask(%u) == 0x%08X", prefixlen, expected_host);
    report(label, got == expected_host);
}

static void test_prefix2netmask(void)
{
    /* A /0 matches everything (mask 0); a /32 is an exact match (all
     * ones). The boundary values and /31 used to trigger undefined
     * shift behavior and produced wrong masks. */
    check_netmask(0, 0x00000000);
    check_netmask(1, 0x80000000);
    check_netmask(8, 0xFF000000);
    check_netmask(16, 0xFFFF0000);
    check_netmask(24, 0xFFFFFF00);
    check_netmask(31, 0xFFFFFFFE);
    check_netmask(32, 0xFFFFFFFF);
    /* anything beyond /32 is treated as an exact match */
    check_netmask(33, 0xFFFFFFFF);
}

/* ------------------------------------------------------------------ */
/* pmix_net_islocalhost                                               */
/* ------------------------------------------------------------------ */

static bool islocalhost_v4(const char *ip)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &sa.sin_addr);
    return pmix_net_islocalhost((struct sockaddr *) &sa);
}

static bool islocalhost_v6(const char *ip)
{
    struct sockaddr_in6 sa;

    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    if (1 != inet_pton(AF_INET6, ip, &sa.sin6_addr)) {
        return false;
    }
    return pmix_net_islocalhost((struct sockaddr *) &sa);
}

static void test_islocalhost(void)
{
    report("islocalhost(127.0.0.1) == true", islocalhost_v4("127.0.0.1"));
    report("islocalhost(127.250.1.2) == true", islocalhost_v4("127.250.1.2"));
    report("islocalhost(10.0.0.1) == false", !islocalhost_v4("10.0.0.1"));
    /* Regression: 255.x must not be mistaken for the 127/8 range */
    report("islocalhost(255.1.2.3) == false", !islocalhost_v4("255.1.2.3"));
    report("islocalhost(::1) == true", islocalhost_v6("::1"));
    report("islocalhost(2001:db8::1) == false", !islocalhost_v6("2001:db8::1"));
    /* an IPv4-mapped address carries an ordinary IPv4 address in its low
     * 32 bits, so the whole of 127.0.0.0/8 names this host through it */
    report("islocalhost(::ffff:127.0.0.1) == true", islocalhost_v6("::ffff:127.0.0.1"));
    report("islocalhost(::ffff:127.9.9.9) == true", islocalhost_v6("::ffff:127.9.9.9"));
    report("islocalhost(::ffff:8.8.8.8) == false", !islocalhost_v6("::ffff:8.8.8.8"));
}

/* ------------------------------------------------------------------ */
/* pmix_net_addr_isipv6linklocal                                       */
/* ------------------------------------------------------------------ */

static void check_linklocal(const char *addr, bool expect)
{
    struct sockaddr_in6 sa;
    char label[96];

    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    if (1 != inet_pton(AF_INET6, addr, &sa.sin6_addr)) {
        snprintf(label, sizeof(label), "inet_pton6(%s)", addr);
        report(label, 0);
        return;
    }
    snprintf(label, sizeof(label), "isipv6linklocal(%s) == %s", addr,
             expect ? "true" : "false");
    report(label, expect == pmix_net_addr_isipv6linklocal((struct sockaddr *) &sa));
}

/* PMIX_ENABLE_IPV6 is 0 in a default build, but the pif IPv6 discovery
 * components are gated on the OS rather than on that flag, so an AF_INET6
 * address is ordinary here. Guarding the AF_INET6 arm on PMIX_ENABLE_IPV6
 * made every one of these answer false - after complaining about an
 * "unhandled sa_family" on stream 0. */
static void test_isipv6linklocal(void)
{
    struct sockaddr_in v4;

    check_linklocal("fe80::1", true);
    check_linklocal("fe80::dead:beef", true);
    check_linklocal("fe81::1", false);
    check_linklocal("2001:db8::1", false);
    check_linklocal("::1", false);

    memset(&v4, 0, sizeof(v4));
    v4.sin_family = AF_INET;
    report("isipv6linklocal(v4) == false",
           !pmix_net_addr_isipv6linklocal((struct sockaddr *) &v4));
}

/* ------------------------------------------------------------------ */
/* pmix_net_get_port                                                  */
/* ------------------------------------------------------------------ */

static void test_get_port(void)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8080);
    report("get_port(8080) == 8080", 8080 == pmix_net_get_port((struct sockaddr *) &sa));
}

/* ------------------------------------------------------------------ */
/* pmix_net_isaddr                                                    */
/* ------------------------------------------------------------------ */

static void test_isaddr(void)
{
    report("isaddr(127.0.0.1) == true", pmix_net_isaddr("127.0.0.1"));
    report("isaddr(::1) == true", pmix_net_isaddr("::1"));
    report("isaddr(256.1.1.1) == false", !pmix_net_isaddr("256.1.1.1"));
    report("isaddr(not-an-address) == false", !pmix_net_isaddr("not-an-address"));
}

/* ------------------------------------------------------------------ */
/* pmix_net_samenetwork                                                */
/*                                                                     */
/* The IPv6 arm used to answer only /64: every other prefix returned   */
/* false regardless of the addresses, so a loopback entry carrying the */
/* /128 the kernel reports never matched even itself.  That is why the */
/* two pif IPv6 discovery components could not agree on what to store  */
/* in if_mask.  The boundary cases below - a prefix that ends inside a */
/* byte, and the 0/128 ends - are the ones a bit-walk gets wrong.      */
/* ------------------------------------------------------------------ */

static void check_v4(const char *a, const char *b, uint32_t plen, bool expect)
{
    struct sockaddr_storage sa, sb;
    char label[128];

    memset(&sa, 0, sizeof(sa));
    memset(&sb, 0, sizeof(sb));
    ((struct sockaddr *) &sa)->sa_family = AF_INET;
    ((struct sockaddr *) &sb)->sa_family = AF_INET;
    if (1 != inet_pton(AF_INET, a, &((struct sockaddr_in *) &sa)->sin_addr)
        || 1 != inet_pton(AF_INET, b, &((struct sockaddr_in *) &sb)->sin_addr)) {
        report("inet_pton failed setting up a v4 case", 0);
        return;
    }
    snprintf(label, sizeof(label), "samenetwork(%s, %s, /%u) == %s", a, b, plen,
             expect ? "true" : "false");
    report(label, expect == pmix_net_samenetwork(&sa, &sb, plen));
}

static void check_v6(const char *a, const char *b, uint32_t plen, bool expect)
{
    struct sockaddr_storage sa, sb;
    char label[128];

    memset(&sa, 0, sizeof(sa));
    memset(&sb, 0, sizeof(sb));
    ((struct sockaddr *) &sa)->sa_family = AF_INET6;
    ((struct sockaddr *) &sb)->sa_family = AF_INET6;
    if (1 != inet_pton(AF_INET6, a, &((struct sockaddr_in6 *) &sa)->sin6_addr)
        || 1 != inet_pton(AF_INET6, b, &((struct sockaddr_in6 *) &sb)->sin6_addr)) {
        report("inet_pton failed setting up a v6 case", 0);
        return;
    }
    snprintf(label, sizeof(label), "samenetwork(%s, %s, /%u) == %s", a, b, plen,
             expect ? "true" : "false");
    report(label, expect == pmix_net_samenetwork(&sa, &sb, plen));
}

static void test_samenetwork(void)
{
    struct sockaddr_storage v4, v6;

    /* IPv4: the arm that already worked, pinned so the rework below
     * cannot disturb it */
    check_v4("192.168.1.10", "192.168.1.200", 24, true);
    check_v4("192.168.1.10", "192.168.2.10", 24, false);
    check_v4("192.168.1.10", "192.168.2.10", 16, true);
    check_v4("10.0.0.1", "10.0.0.1", 32, true);
    check_v4("10.0.0.1", "10.0.0.2", 32, false);
    /* a prefix that ends inside a byte, and the two ends */
    check_v4("10.1.0.0", "10.1.63.255", 18, true);
    check_v4("10.1.0.0", "10.1.64.0", 18, false);
    check_v4("1.2.3.4", "250.250.250.250", 0, false); /* 0 means /32 for v4 */
    check_v4("1.2.3.4", "250.250.250.250", 1, false);
    check_v4("1.2.3.4", "5.6.7.8", 40, false);        /* clamped to /32 */

    /* IPv6 /64: what the old code did, and all it did.  These must not
     * change. */
    check_v6("2001:db8:1:2::1", "2001:db8:1:2::abcd", 64, true);
    check_v6("2001:db8:1:2::1", "2001:db8:1:3::1", 64, false);
    check_v6("2001:db8:1:2::1", "2001:db8:1:2::abcd", 0, true); /* 0 => /64 */

    /* IPv6 at every other prefix: each of these returned false before,
     * whatever the addresses were. */
    check_v6("::1", "::1", 128, true);   /* the loopback case linux_ipv6 hits */
    check_v6("::1", "::2", 128, false);
    check_v6("2001:db8:1:2::1", "2001:db8:1:3::9", 48, true);
    check_v6("2001:db8:1:2::1", "2001:db8:2:3::9", 48, false);
    check_v6("2001:db8::1", "2001:db8::2", 127, false);
    check_v6("2001:db8::2", "2001:db8::3", 127, true);
    /* prefixes that end inside a byte: 2001:0db8 / 2001:0dba / 2001:0dc8
     * share three leading bytes and differ in the fourth */
    check_v6("2001:db8::1", "2001:dba::1", 24, true);  /* byte-aligned, before the difference */
    check_v6("2001:db8::1", "2001:dc8::1", 24, true);  /* likewise - /24 never reaches it */
    check_v6("2001:db8::1", "2001:dba::1", 28, true);  /* 0xb8 & 0xf0 == 0xba & 0xf0 */
    check_v6("2001:db8::1", "2001:dc8::1", 28, false); /* 0xb0 != 0xc0 */
    check_v6("2001:db8::1", "2001:dba::1", 32, false); /* now the whole byte counts */
    check_v6("::", "ffff::", 1, false);
    check_v6("8000::", "ffff::", 1, true);
    check_v6("2001:db8::1", "fe80::1", 150, false);    /* clamped to /128 */

    /* mismatched families never match, whatever the prefix */
    memset(&v4, 0, sizeof(v4));
    memset(&v6, 0, sizeof(v6));
    ((struct sockaddr *) &v4)->sa_family = AF_INET;
    ((struct sockaddr *) &v6)->sa_family = AF_INET6;
    report("samenetwork(v4, v6, /0) == false", !pmix_net_samenetwork(&v4, &v6, 0));
    report("samenetwork(v4, v6, /64) == false", !pmix_net_samenetwork(&v4, &v6, 64));
}

/* ------------------------------------------------------------------ */
/* pmix_net_addr_isipv4public                                          */
/* ------------------------------------------------------------------ */

static void check_public(const char *addr, bool expect)
{
    struct sockaddr_in sa;
    char label[96];

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (1 != inet_pton(AF_INET, addr, &sa.sin_addr)) {
        snprintf(label, sizeof(label), "inet_pton(%s)", addr);
        report(label, 0);
        return;
    }
    snprintf(label, sizeof(label), "isipv4public(%s) == %s", addr,
             expect ? "true" : "false");
    report(label, expect == pmix_net_addr_isipv4public((struct sockaddr *) &sa));
}

/* The table of private networks comes from an MCA parameter, and the
 * parameter system does not have the value until pmix_register_params()
 * runs - which is well after pmix_init_util(), where the network helper
 * is initialized. Parsing it at pmix_net_init() time therefore saw only
 * the NULL initializer and left every address, RFC1918 included, looking
 * public; the MCA parameter was inert. Everything below answers "public"
 * if that regresses.
 *
 * The list is supplied through the environment rather than left at its
 * default so that the test also proves the *user's* value is the one
 * being consulted: 172.16.0.0/12 is private by default and public here. */
static void test_isipv4public(void)
{
    struct sockaddr_in6 sa6;

    setenv("PMIX_MCA_pmix_net_private_ipv4", "10.0.0.0/8;192.168.0.0/16", 1);

    if (PMIX_SUCCESS != pmix_init_util(NULL, 0, NULL)) {
        report("pmix_init_util", 0);
        return;
    }
    if (PMIX_SUCCESS != pmix_register_params()) {
        report("pmix_register_params", 0);
        return;
    }

    check_public("10.1.2.3", false);
    check_public("192.168.1.1", false);
    check_public("172.16.5.5", true);
    check_public("8.8.8.8", true);

    /* an IPv6 address is never an IPv4 public address */
    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    report("isipv4public(::1) == false",
           !pmix_net_addr_isipv4public((struct sockaddr *) &sa6));
}

#endif /* HAVE_STRUCT_SOCKADDR_IN */

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_net unit tests ===\n\n");

#if defined(HAVE_STRUCT_SOCKADDR_IN)
    test_prefix2netmask();
    test_islocalhost();
    test_get_port();
    test_isaddr();
    test_samenetwork();
    test_isipv6linklocal();
    /* last - it initializes the library */
    test_isipv4public();
#else
    fprintf(stdout, "  SKIP: no struct sockaddr_in on this platform\n");
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
