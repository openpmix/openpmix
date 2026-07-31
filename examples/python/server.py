#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/server.c
#
# A minimal but complete PMIx server: it registers a namespace, launches
# some clients into it, services their requests through the server-module
# upcalls, and shuts down when they have all finalized.
#
#   ./server.py [-n <nprocs>] [-e <executable> [args...]]
#
# so, for example:
#
#   ./server.py -n 2 -e ./client.py
#
# The upcall handlers are the Python counterparts of the C module's
# function pointers. The binding invokes each with three arguments: the
# converted request, a wrapper around the C completion callback, and an
# opaque cbdata. A handler drives completion by invoking that callback and
# returning PMIX_SUCCESS.
#
# Two structural differences from the C version are worth noting:
#   - the C version watches for SIGCHLD through libevent to reap clients;
#     here the children are subprocess.Popen objects and the finalized
#     upcall is what counts them down, exactly as the C version's
#     "wakeup" counter does.
#   - PMIx_Info_list_* and the pmix_list_t published-data list have no
#     Python counterpart; a dict does the same job.

import os
import shutil
import socket
import subprocess
import sys
import time

from examples import *

server = PMIxServer()

# how many clients still have to finalize before we may exit
wakeup = 0
# where published data lives, keyed by the published key
pubdata = {}


def ok(rc):
    """True for both flavors of success.

    Several server-side APIs report "done, and there was nothing to wait
    for" as PMIX_OPERATION_SUCCEEDED rather than PMIX_SUCCESS.
    """
    return PMIX_SUCCESS == rc or PMIX_OPERATION_SUCCEEDED == rc


def normalize_key(ky):
    """Keys reach a handler as bytes; the client hands us str."""
    if isinstance(ky, bytes):
        return ky.decode('ascii')
    return ky


def connected(proc, cbfunc, cbdata):
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def finalized(proc, cbfunc, cbdata):
    global wakeup

    eprint("SERVER: FINALIZED %s:%d" % (proc['nspace'], proc['rank']))
    wakeup -= 1
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def abort_fn(args, cbfunc, cbdata):
    procs = args.get('procs')
    if procs:
        eprint("SERVER: ABORT on %s:%d"
               % (procs[0]['nspace'], procs[0]['rank']))
    else:
        eprint("SERVER: ABORT OF ALL PROCS IN NSPACE %s"
               % args['proc']['nspace'])

    # send a notification as the C version does, tagged with a pair of
    # arbitrary values so we can see it arrive
    info = [{'key': "DARTH", 'value': 12, 'val_type': PMIX_INT8},
            {'key': "VADER", 'value': 12.34, 'val_type': PMIX_FLOAT}]
    rc = server.notify_event(PMIX_ERR_JOB_TERMINATED, args['proc'],
                             PMIX_RANGE_NAMESPACE, info)
    if PMIX_SUCCESS != rc:
        eprint("SERVER: FAILED NOTIFY ERROR %d" % rc)

    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def fencenb_fn(args, cbfunc, cbdata):
    eprint("SERVER: FENCENB")
    # pass back a dummy buffer - we don't collect any data here
    cbfunc(PMIX_SUCCESS, bytearray(0), cbdata)
    return PMIX_SUCCESS


def dmodex_fn(args, cbfunc, cbdata):
    eprint("SERVER: DMODEX")
    # we don't have any data for remote procs as this
    # test only runs one server - so report accordingly
    cbfunc(PMIX_ERR_NOT_FOUND, bytearray(0), cbdata)
    return PMIX_SUCCESS


def publish_fn(args, cbfunc, cbdata):
    eprint("SERVER: PUBLISH")
    for d in args['directives']:
        key = normalize_key(d['key'])
        if "pmix" in key:
            # a directive qualifying the request, not data to publish
            continue
        pubdata[key] = {'proc': args['proc'], 'value': d['value'],
                        'val_type': d['val_type']}
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def lookup_fn(args, cbfunc, cbdata):
    eprint("SERVER: LOOKUP")
    pdata = []
    for ky in args['keys']:
        ky = normalize_key(ky)
        if ky in pubdata:
            d = pubdata[ky]
            pdata.append({'proc': d['proc'], 'key': ky,
                          'value': d['value'], 'val_type': d['val_type']})
    if 0 == len(pdata):
        cbfunc(PMIX_ERR_NOT_FOUND, None, cbdata)
    else:
        cbfunc(PMIX_SUCCESS, pdata, cbdata)
    return PMIX_SUCCESS


def unpublish_fn(args, cbfunc, cbdata):
    eprint("SERVER: UNPUBLISH")
    keys = args['keys']
    if keys:
        for ky in keys:
            pubdata.pop(normalize_key(ky), None)
    else:
        # no keys means "remove everything this proc published"
        pubdata.clear()
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def spawn_fn(args, cbfunc, cbdata):
    eprint("SERVER: SPAWN")

    # in practice, we would pass this request to the local resource
    # manager for launch, and then have that server execute our callback
    # function. For now, we will fake the spawn and just pretend - but we
    # must register the nspace for the new procs before we return to the
    # caller
    set_namespace(2, "0,1", "DYNSPACE")
    cbfunc(PMIX_SUCCESS, "DYNSPACE", cbdata)
    return PMIX_SUCCESS


def connect_fn(args, cbfunc, cbdata):
    eprint("SERVER: CONNECT")
    # in practice, we would pass this request to the local
    # resource manager for handling
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def disconnect_fn(args, cbfunc, cbdata):
    eprint("SERVER: DISCONNECT")
    # in practice, we would pass this request to the local
    # resource manager for handling
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def register_events(args, cbfunc, cbdata):
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def deregister_events(args, cbfunc, cbdata):
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


def notify_event(args, cbfunc, cbdata):
    return PMIX_SUCCESS


def query_fn(args, cbfunc, cbdata):
    eprint("SERVER: QUERY")
    # keep this simple - answer each query with its own index
    info = []
    for n, q in enumerate(args['queries']):
        info.append({'key': normalize_key(q['keys'][0]), 'value': "%d" % n,
                     'val_type': PMIX_STRING})
    cbfunc(PMIX_SUCCESS, info, cbdata)
    return PMIX_SUCCESS


def tool_connect_fn(args, cbfunc, cbdata):
    eprint("SERVER: TOOL CONNECT")
    # just pass back an arbitrary nspace
    cbfunc(PMIX_SUCCESS, {'nspace': "TOOL", 'rank': 0}, cbdata)
    return PMIX_SUCCESS


def log_fn(args, cbfunc, cbdata):
    eprint("SERVER: LOG")
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS


mymodule = {'clientconnected': connected,
            'clientfinalized': finalized,
            'abort': abort_fn,
            'fencenb': fencenb_fn,
            'directmodex': dmodex_fn,
            'publish': publish_fn,
            'lookup': lookup_fn,
            'unpublish': unpublish_fn,
            'spawn': spawn_fn,
            'connect': connect_fn,
            'disconnect': disconnect_fn,
            'registerevents': register_events,
            'deregisterevents': deregister_events,
            'notifyevent': notify_event,
            'query': query_fn,
            'toolconnected': tool_connect_fn,
            'log': log_fn}


def errhandler(evhdlr, status, source, info, results):
    eprint("SERVER: ERRHANDLER CALLED WITH STATUS %d" % status)
    return PMIX_EVENT_ACTION_COMPLETE, None


def set_namespace(nprocs, ranks, nspace):
    """Describe a namespace to the server library and register it."""
    hostname = socket.gethostname()

    # request application setup information - e.g., network security keys
    # or endpoint info
    rc, appinfo = server.setup_application(nspace, [])
    if PMIX_SUCCESS != rc:
        eprint("Failed to setup application: %d" % rc)
        sys.exit(1)

    info = list(appinfo) if appinfo else []

    # both generators hand back a bytearray; the info value is a string
    rc, regex = server.generate_regex(hostname.split(','))
    rc2, ppn = server.generate_ppn(ranks.split(';'))
    regex = bytes(regex).decode('ascii')
    ppn = bytes(ppn).decode('ascii')

    info += [{'key': PMIX_UNIV_SIZE, 'value': nprocs,
              'val_type': PMIX_UINT32},
             {'key': PMIX_SPAWNED, 'value': 0, 'val_type': PMIX_UINT32},
             {'key': PMIX_LOCAL_SIZE, 'value': nprocs,
              'val_type': PMIX_UINT32},
             {'key': PMIX_LOCAL_PEERS, 'value': ranks,
              'val_type': PMIX_STRING},
             {'key': PMIX_NODE_MAP, 'value': regex,
              'val_type': PMIX_STRING},
             {'key': PMIX_PROC_MAP, 'value': ppn, 'val_type': PMIX_STRING},
             {'key': PMIX_JOB_SIZE, 'value': nprocs,
              'val_type': PMIX_UINT32}]

    return server.register_nspace(nspace, nprocs, info)


def main():
    global wakeup

    # define and pass a personal tmpdir to protect the system
    uid = os.geteuid()
    tdir = os.environ.get('TMPDIR') or os.environ.get('TEMP') \
        or os.environ.get('TMP') or "/tmp"
    tmpdir = os.path.join(tdir, "pmix.%lu" % uid)
    # create the directory
    try:
        os.makedirs(tmpdir, mode=0o700, exist_ok=True)
    except OSError:
        eprint("Cannot make tmpdir %s" % tmpdir)
        sys.exit(1)

    # setup the server library
    info = [{'key': PMIX_SERVER_TMPDIR, 'value': tmpdir,
             'val_type': PMIX_STRING}]
    rc = server.init(info, mymodule)
    if PMIX_SUCCESS != rc:
        eprint("Init failed with error %d" % rc)
        return rc

    # register the errhandler
    server.register_event_handler(None, None, errhandler)

    # see if we were passed the number of procs to run or the executable
    # to use
    nprocs = 1
    executable = None
    client_argv = []
    n = 1
    while n < len(sys.argv):
        if "-n" == sys.argv[n] and n + 1 < len(sys.argv):
            nprocs = int(sys.argv[n + 1])
            n += 1
        elif "-e" == sys.argv[n] and n + 1 < len(sys.argv):
            executable = sys.argv[n + 1]
            client_argv = sys.argv[n + 2:]
            n = len(sys.argv)
        n += 1
    if executable is None:
        executable = "./simpclient"

    # we have a single namespace for all clients
    ranks = ','.join(str(n) for n in range(nprocs))

    # register the nspace
    rc = set_namespace(nprocs, ranks, "foobar")
    if not ok(rc):
        eprint("Register nspace failed: %d" % rc)
        return cleanup(tmpdir, rc)

    wakeup = nprocs
    myuid = os.getuid()
    mygid = os.getgid()

    # prep the local node for launch
    rc = server.setup_local_support("foobar", [])
    if not ok(rc):
        eprint("Setup local support failed: %d" % rc)
        return cleanup(tmpdir, rc)

    # fork/exec the test
    children = []
    for n in range(nprocs):
        proc = {'nspace': "foobar", 'rank': n}
        client_env = dict(os.environ)
        rc = server.setup_fork(proc, client_env)
        if not ok(rc):
            eprint("Server fork setup failed with error %d" % rc)
            return cleanup(tmpdir, rc)
        # don't fork/exec the client until we know it is registered
        # so we avoid a potential race condition in the server
        rc = server.register_client(proc, myuid, mygid)
        if not ok(rc):
            eprint("Server fork setup failed with error %d" % rc)
            return cleanup(tmpdir, rc)
        children.append(subprocess.Popen([executable] + client_argv,
                                         env=client_env))

    # hang around until the client(s) finalize. The library runs its own
    # progress thread, so we only have to wait - calling progress() here
    # would re-enter the event base
    while 0 < wakeup:
        time.sleep(0.0001)

    for child in children:
        child.wait()

    # deregister the errhandler
    server.deregister_event_handler(0)

    # release any pub data
    pubdata.clear()

    # finalize the server library
    rc = server.finalize()
    if PMIX_SUCCESS != rc:
        eprint("Finalize failed with error %d" % rc)

    eprint("Test finished OK!")
    return cleanup(tmpdir, rc)


def cleanup(tmpdir, rc):
    shutil.rmtree(tmpdir, ignore_errors=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
