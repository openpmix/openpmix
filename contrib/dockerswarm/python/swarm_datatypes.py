#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Push every data type the Python conversion layer supports through a real
# multi-node put/commit/fence/get, and check that what a peer reads back is
# what was written.
#
# Why this cannot be a unit test.  test_bindings.py exercises the loader and
# the unloader against each other through the pmix_value_roundtrip hook, which
# is necessary but not sufficient: a mistake both halves make - the element
# *size* of an array, say, or the byte order of a coordinate vector - cancels
# out and looks like a clean round trip.  The authority for a layout is the C
# side that has to read it.  Sending a value to a peer behind a *different*
# prted forces it through pmix_bfrops_base_pack_*/unpack_*, so a layout the
# bindings got wrong stops matching.
#
# The cases here are chosen to be the ones that would silently pass a
# same-process round trip:
#
#   * binary payloads - byte objects carrying NUL and high bytes, which a
#     converter that treats them as C strings truncates
#   * every width at its largest representable value, which a bounds check
#     written with the wrong constant either refuses or silently truncates
#   * data arrays, including empty ones and nested ones
#   * the structured types (proc, proc_info, envar, coord) whose members are
#     read back by field
#
# Prints "PMIXPY <rank> <PASS|FAIL> <name>" per check and a final DONE line,
# exactly as the other swarm clients do.  Exits non-zero if any check failed.

from pmix import *
import sys

myrank = -1
nfail = 0


def check(name, ok, detail=""):
    global nfail
    if not ok:
        nfail += 1
    print("PMIXPY %d %s %s%s" % (myrank, "PASS" if ok else "FAIL", name,
                                 ("  [" + str(detail) + "]") if detail else ""),
          flush=True)


# A coordinate vector crosses as the raw uint32 block, so build it the way
# the bindings hand it back
def coord(*vals):
    return {'view': 1,
            'coord': b''.join(v.to_bytes(4, sys.byteorder) for v in vals),
            'dims': len(vals)}


# key suffix, value dict.  The key is qualified with the writing rank so
# every rank publishes its own copy and no two collide.
CASES = (
    ("bool", {'value': True, 'val_type': PMIX_BOOL}),
    ("byte", {'value': 200, 'val_type': PMIX_BYTE}),
    ("string", {'value': "a string", 'val_type': PMIX_STRING}),
    ("size", {'value': 1 << 40, 'val_type': PMIX_SIZE}),
    ("int", {'value': -7, 'val_type': PMIX_INT}),
    ("int8", {'value': -128, 'val_type': PMIX_INT8}),
    ("int16", {'value': 32767, 'val_type': PMIX_INT16}),
    ("int32", {'value': -2147483648, 'val_type': PMIX_INT32}),
    # the int64/uint64 bounds checks used to be written as products that
    # were nowhere near the real limits, so the top of the range was
    # refused outright
    ("int64", {'value': 9223372036854775807, 'val_type': PMIX_INT64}),
    ("uint", {'value': 4294967295, 'val_type': PMIX_UINT}),
    ("uint8", {'value': 255, 'val_type': PMIX_UINT8}),
    ("uint16", {'value': 65535, 'val_type': PMIX_UINT16}),
    ("uint32", {'value': 4294967295, 'val_type': PMIX_UINT32}),
    ("uint64", {'value': 18446744073709551615, 'val_type': PMIX_UINT64}),
    ("double", {'value': 3.140625, 'val_type': PMIX_DOUBLE}),
    ("timeval", {'value': {'sec': 12, 'usec': 34}, 'val_type': PMIX_TIMEVAL}),
    ("status", {'value': PMIX_ERR_NOT_FOUND, 'val_type': PMIX_STATUS}),
    ("rank", {'value': 3, 'val_type': PMIX_PROC_RANK}),
    ("proc", {'value': {'nspace': "somens", 'rank': 2},
              'val_type': PMIX_PROC}),
    # a byte object whose payload is neither NUL-free nor NUL-terminated -
    # the shape a converter that runs strlen over it gets wrong
    ("bo-binary", {'value': {'bytes': bytes(range(256)), 'size': 256},
                   'val_type': PMIX_BYTE_OBJECT}),
    ("bo-empty", {'value': {'bytes': b'', 'size': 0},
                  'val_type': PMIX_BYTE_OBJECT}),
    ("envar", {'value': {'envar': "SWARM_VAR", 'value': "one:two",
                         'separator': ':'}, 'val_type': PMIX_ENVAR}),
    ("procinfo", {'value': {'proc': {'nspace': "somens", 'rank': 1},
                            'hostname': "somehost",
                            'executable': "someexec",
                            'pid': 4321, 'exitcode': 0, 'state': 2},
                  'val_type': PMIX_PROC_INFO}),
    ("coord", {'value': coord(1, 2, 3), 'val_type': PMIX_COORD}),
    # arrays, including the empty one that used to raise TypeError coming
    # back out of the unloader
    ("array-int", {'value': {'type': PMIX_INT32, 'array': [1, -2, 3]},
                   'val_type': PMIX_DATA_ARRAY}),
    ("array-string", {'value': {'type': PMIX_STRING, 'array': ["a", "bb"]},
                      'val_type': PMIX_DATA_ARRAY}),
    ("array-bool", {'value': {'type': PMIX_BOOL, 'array': [True, False, True]},
                    'val_type': PMIX_DATA_ARRAY}),
    ("array-size", {'value': {'type': PMIX_SIZE, 'array': [0, 1 << 32]},
                    'val_type': PMIX_DATA_ARRAY}),
    ("array-empty", {'value': {'type': PMIX_INT, 'array': []},
                     'val_type': PMIX_DATA_ARRAY}),
    ("array-proc", {'value': {'type': PMIX_PROC,
                              'array': [{'nspace': "n1", 'rank': 0},
                                        {'nspace': "n2", 'rank': 1}]},
                    'val_type': PMIX_DATA_ARRAY}),
    ("array-bo", {'value': {'type': PMIX_BYTE_OBJECT,
                            'array': [{'bytes': b'\x00\xff', 'size': 2},
                                      {'bytes': b'abc', 'size': 3}]},
                  'val_type': PMIX_DATA_ARRAY}),
    ("array-nested", {'value': {'type': PMIX_DATA_ARRAY, 'array': [
                          {'type': PMIX_INT, 'array': [1, 2]},
                          {'type': PMIX_STRING, 'array': ["x"]}]},
                      'val_type': PMIX_DATA_ARRAY}),
)


def same(want, got):
    """Compare a written value with what came back.

    Two allowances, both of them the library's doing rather than the
    bindings': a string may arrive as bytes, and a byte object reports the
    length of the payload it actually carries.
    """
    if isinstance(want, bytes) and isinstance(got, str):
        got = got.encode('ascii')
    if isinstance(want, str) and isinstance(got, bytes):
        got = got.decode('ascii')
    if isinstance(want, dict) and isinstance(got, dict):
        if set(want.keys()) != set(got.keys()):
            return False
        return all(same(want[k], got[k]) for k in want)
    if isinstance(want, list) and isinstance(got, list):
        if len(want) != len(got):
            return False
        return all(same(a, b) for a, b in zip(want, got))
    return want == got


def main():
    global myrank

    client = PMIxClient()
    rc, myproc = client.init([])
    if PMIX_SUCCESS != rc:
        print("PMIXPY -1 FAIL init [%d]" % rc, flush=True)
        return 1
    myrank = myproc['rank']
    nspace = myproc['nspace']
    check("init", True, "%s:%d" % (nspace, myrank))

    jobproc = {'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(jobproc, PMIX_JOB_SIZE, [])
    if PMIX_SUCCESS != rc or val is None:
        check("get PMIX_JOB_SIZE", False, rc)
        client.finalize([])
        return 1
    nprocs = val['value']
    check("job size >= 2 (multi-rank run)", nprocs >= 2, nprocs)

    # publish one key per case, under global scope so peers can read them
    put_ok = True
    for name, value in CASES:
        rc = client.put(PMIX_GLOBAL, "swarm.dt.%s" % name, dict(value))
        if PMIX_SUCCESS != rc:
            put_ok = False
            check("put %s" % name, False, rc)
    check("put every data type", put_ok, "%d types" % len(CASES))

    rc = client.commit()
    check("commit", PMIX_SUCCESS == rc, rc)
    rc = client.fence([{'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}], [])
    check("fence", PMIX_SUCCESS == rc, rc)

    # Read every case back from every OTHER rank.  A peer behind a different
    # prted is the whole point: that value was packed by the library, sent
    # over the wire, and unpacked on this side.
    for name, value in CASES:
        ok = True
        detail = ""
        for peer in range(nprocs):
            if peer == myrank:
                continue
            p = {'nspace': nspace, 'rank': peer}
            rc, got = client.get(p, "swarm.dt.%s" % name, [])
            if PMIX_SUCCESS != rc or got is None:
                ok = False
                detail = "peer %d rc=%d" % (peer, rc)
                break
            if not same(value['value'], got['value']):
                ok = False
                detail = "peer %d wanted %r got %r" % (peer, value['value'],
                                                       got['value'])
                break
        check("remote get %s" % name, ok, detail)

    # Serialization of the same values, which is the other consumer of the
    # conversion layer.  data_pack hands the value to the library's packer
    # directly, so a layout mistake shows up here without a peer at all -
    # but it needs an initialized library, which is why it lives beside the
    # connected checks rather than in the unit suite.
    pack_ok = True
    detail = ""
    for name, value in CASES:
        rc, buf = client.data_pack(None, dict(value), PMIX_VALUE)
        if PMIX_SUCCESS != rc:
            pack_ok = False
            detail = "%s pack rc=%d" % (name, rc)
            break
        rc, got = client.data_unpack(buf, PMIX_VALUE)
        if PMIX_SUCCESS != rc or got is None:
            pack_ok = False
            detail = "%s unpack rc=%d" % (name, rc)
            break
        if not same(value['value'], got['value']):
            pack_ok = False
            detail = "%s wanted %r got %r" % (name, value['value'],
                                              got['value'])
            break
    check("pack/unpack every data type", pack_ok, detail)

    # the same payloads through the compressor, which is the one consumer
    # that cares about every byte rather than about the type
    rc, comp = client.data_compress(bytes(range(256)) * 64)
    if PMIX_SUCCESS == rc:
        rc2, back = client.data_decompress(comp)
        check("compress/decompress round trip",
              PMIX_SUCCESS == rc2 and back == bytes(range(256)) * 64,
              "rc=%d" % rc2)
    else:
        # no compression component built is a legitimate answer
        check("compress declined cleanly", PMIX_ERR_NOT_AVAILABLE == rc, rc)

    rc = client.finalize([])
    check("finalize", PMIX_SUCCESS == rc, rc)
    print("PMIXPY %d DONE %d" % (myrank, nfail), flush=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
