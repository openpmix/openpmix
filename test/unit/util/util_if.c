/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pure address/mask parser in src/util/pmix_if.c
 * (pmix_iftupletoaddr).  These need no populated interface list, so they
 * run without any server/tool initialization.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/util/pmix_if.h"

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

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
