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

static void test_islocalhost(void)
{
    report("islocalhost(127.0.0.1) == true", islocalhost_v4("127.0.0.1"));
    report("islocalhost(127.250.1.2) == true", islocalhost_v4("127.250.1.2"));
    report("islocalhost(10.0.0.1) == false", !islocalhost_v4("10.0.0.1"));
    /* Regression: 255.x must not be mistaken for the 127/8 range */
    report("islocalhost(255.1.2.3) == false", !islocalhost_v4("255.1.2.3"));
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
#else
    fprintf(stdout, "  SKIP: no struct sockaddr_in on this platform\n");
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
