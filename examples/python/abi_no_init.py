#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/abi_no_init.c
#
# Test query outside of init/finalize by asking for keys (the ABI
# versions) that are allowed to be accessed outside of the init/finalize
# region.

import sys

from examples import *


def main():
    client = PMIxClient()

    queries = [{'keys': [PMIX_QUERY_STABLE_ABI_VERSION], 'qualifiers': None},
               {'keys': [PMIX_QUERY_PROVISIONAL_ABI_VERSION],
                'qualifiers': None}]

    rc, info = client.query(queries)
    if PMIX_SUCCESS != rc:
        eprint("Error: PMIx_Query_info failed: %d (%s)"
               % (rc, client.error_string(rc)))
        return rc

    print("--> Query returned (ninfo %d)" % len(info))
    for item in info:
        print("--> KEY: %s" % item['key'])
        if PMIX_QUERY_STABLE_ABI_VERSION == item['key']:
            print("----> ABI (Stable): String: %s" % item['value'])
        elif PMIX_QUERY_PROVISIONAL_ABI_VERSION == item['key']:
            print("----> ABI (Provisional): String: %s" % item['value'])

    return 0


if __name__ == '__main__':
    sys.exit(main())
