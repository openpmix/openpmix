#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/abi_with_init.c
#
# Test query with locally resolved keys (the ABI versions) and a key that
# the server will need to resolve, in both the blocking and the
# non-blocking form.

import sys

from examples import *


def cbfunc(status, results, q):
    q.status = status
    q.info = results
    q.wakeup()


def report(client, label, status, info):
    print("--> %s returned %s (ninfo %d)"
          % (label, client.error_string(status), len(info)))
    for item in info:
        print("--> KEY: %s" % item['key'])
        if PMIX_QUERY_STABLE_ABI_VERSION == item['key']:
            print("----> ABI (Stable): String: %s" % item['value'])
        elif PMIX_QUERY_PROVISIONAL_ABI_VERSION == item['key']:
            print("----> ABI (Provisional): String: %s" % item['value'])
        elif PMIX_QUERY_NAMESPACES == item['key']:
            print("----> Namespaces: String: %s" % item['value'])


def main():
    client = PMIxClient()

    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH != rc:
            eprint("PMIx_Init failed:", client.error_string(rc))
            sys.exit(rc)

    queries = [{'keys': [PMIX_QUERY_STABLE_ABI_VERSION], 'qualifiers': None},
               {'keys': [PMIX_QUERY_PROVISIONAL_ABI_VERSION],
                'qualifiers': None},
               {'keys': [PMIX_QUERY_NAMESPACES], 'qualifiers': None}]

    rc, info = client.query(queries)
    if PMIX_SUCCESS != rc:
        eprint("Error: PMIx_Query_info failed: %d (%s)"
               % (rc, client.error_string(rc)))
        return rc
    report(client, "Query", rc, info)

    # now do it with the non-blocking form
    q = MyQuery()
    rc = client.query_nb(queries, cbfunc, q)
    if PMIX_SUCCESS != rc:
        eprint("Error: PMIx_Query_info_nb failed: %d (%s)"
               % (rc, client.error_string(rc)))
        return rc
    q.wait()
    report(client, "Query_nb", q.status, q.info)

    return client.finalize(None)


if __name__ == '__main__':
    sys.exit(main())
