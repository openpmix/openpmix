#!/usr/bin/env python3

from pmix import *
import signal, time
import os
import select
import sys
import subprocess
import threading

global killer

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

# Server-module upcall handlers. The binding invokes each registered
# handler with three arguments: the converted request (proc/args dict),
# a Python wrapper around the C completion callback, and an opaque cbdata
# dict. A handler drives completion by invoking that callback and returns
# PMIX_SUCCESS. The full set of upcalls, and the wiring that dispatches
# them, is in the "permitted" list of setmodulefn in bindings/python/pmix.pyx.
def clientconnected(proc, cbfunc, cbdata):
    print("CLIENT CONNECTED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

def clientfinalized(proc, cbfunc, cbdata):
    print("CLIENT FINALIZED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

# A trivially small published-data store so the client can exercise the
# publish/lookup/unpublish path. Keyed by the published key, holding the
# publishing proc alongside the value.
published = {}

def normalize_key(ky):
    # keys reach a handler as bytes, but the client hands us str
    if isinstance(ky, bytes):
        return ky.decode('ascii')
    return ky

def clientpublish(args, cbfunc, cbdata):
    print("PUBLISH", args['proc'], args['directives'])
    noreturn = False
    for d in args['directives']:
        if normalize_key(d['key']).startswith('pytest.noreturn'):
            noreturn = True
        if normalize_key(d['key']).startswith('pytest.badstatus'):
            # a completion given something that is not a status still
            # answers the requester - as a failure. It is made from a
            # timer, after the handler has returned: an exception there
            # would end the thread and leave the request unanswered
            threading.Timer(0.01, cbfunc, ["not a status", cbdata]).start()
            return PMIX_SUCCESS
        if 'pmix' in d['key']:
            # a directive qualifying the request, not data to publish
            continue
        published[normalize_key(d['key'])] = {'proc': args['proc'],
                                              'value': d['value'],
                                              'val_type': d['val_type']}
    cbfunc(PMIX_SUCCESS, cbdata)
    if noreturn:
        # A handler that completes and then forgets to return a status.
        # The library has already acted on the completion and released
        # the request, so the binding must tell it PMIX_SUCCESS - an error
        # here would have it release the request a second time
        return
    return PMIX_SUCCESS

def clientlookup(args, cbfunc, cbdata):
    print("LOOKUP", args['proc'], args['keys'])
    keys = [normalize_key(k) for k in args['keys']]
    # A handler answering with something the bindings cannot convert.
    # The requester must still be answered - with an error - rather than
    # left waiting for a completion that never comes
    if 'pytest.generator' in keys:
        # an answer with no length, given after the helper has already
        # taken the completion: the requester must still be answered
        cbfunc(PMIX_SUCCESS, (d for d in []), cbdata)
        return PMIX_SUCCESS
    if 'pytest.badvalue' in keys:
        cbfunc(PMIX_SUCCESS, [{'proc': args['proc'], 'key': 'pytest.badvalue',
                               'value': object(), 'val_type': PMIX_STRING}],
               cbdata)
        return PMIX_SUCCESS
    pdata = []
    for ky in keys:
        if ky in published:
            d = published[ky]
            pdata.append({'proc': d['proc'], 'key': ky,
                          'value': d['value'], 'val_type': d['val_type']})
    if 0 == len(pdata):
        cbfunc(PMIX_ERR_NOT_FOUND, None, cbdata)
    else:
        cbfunc(PMIX_SUCCESS, pdata, cbdata)
    # a handler that completes and then raises: the requester has its
    # answer, and must keep it
    if 'pytest.raiseafter' in keys:
        raise RuntimeError("raised after completing - expected by the test")
    return PMIX_SUCCESS

def clientunpublish(args, cbfunc, cbdata):
    print("UNPUBLISH", args['proc'], args['keys'])
    for ky in args['keys']:
        published.pop(normalize_key(ky), None)
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

def clientfence(args, cbfunc, cbdata):
    # check directives
    try:
        if args['directives'] is not None:
            for d in args['directives']:
                # these are each an info dict
                if "pmix" not in d['key']:
                    # we do not support such directives - see if
                    # it is required
                    try:
                        if d['flags'] & PMIX_INFO_REQD:
                            # return an error
                            return PMIX_ERR_NOT_SUPPORTED
                    except:
                        #it can be ignored
                        pass
    except:
        pass
    # no data to return from this (empty) fence - and None is how a
    # handler says so (sched.py passes an empty bytearray instead, so
    # both spellings stay covered)
    cbfunc(PMIX_SUCCESS, None, cbdata)
    return PMIX_SUCCESS

# The PMIx constants are bytes while the keys handed to a handler are
# str, so normalize before comparing them
def keymatch(attr, key):
    if isinstance(attr, bytes):
        attr = attr.decode('ascii')
    if isinstance(key, bytes):
        key = key.decode('ascii')
    return attr == key

# Group operations are the one server-module upcall whose completion is
# deliberately deferred here. The library allows the host to answer after
# the handler has returned, and a real resource manager generally must -
# it has to hear from its peers first. Completing from a timer thread
# keeps that path under test.
#
# The host is responsible for reporting the final membership: the server
# library relays it to the participants, and each participant records the
# group locally on the strength of it. Omit it and a later leave/destruct
# fails with PMIX_ERR_NOT_FOUND.
def clientgroup(args, cbfunc, cbdata):
    print("GROUP", args['op'], args['group'], args['procs'])
    results = [{'key': PMIX_GROUP_MEMBERSHIP,
                'value': {'type': PMIX_PROC, 'array': args['procs']},
                'val_type': PMIX_DATA_ARRAY}]
    # if the group asked for a context ID, make one up - a real host would
    # allocate it from a system-wide space
    for d in args.get('directives', []):
        if keymatch(PMIX_GROUP_ASSIGN_CONTEXT_ID, d['key']):
            results.append({'key': PMIX_GROUP_CONTEXT_ID,
                            'value': 42, 'val_type': PMIX_SIZE})
    threading.Timer(0.001, cbfunc, [PMIX_SUCCESS, results, cbdata]).start()
    return PMIX_SUCCESS

# An abort the client asks for names a process that does not exist, so
# there is nothing to kill: what is checked is that the request reaches
# abort2 carrying the requester's uid and gid, each once. Anything else
# is refused, which fails the client.
def clientabort2(args, cbfunc, cbdata):
    print("ABORT2", args['caller'], args.get('targets'), args.get('msg'))
    uids = [i['value'] for i in args.get('info', []) if keymatch(PMIX_USERID, i['key'])]
    gids = [i['value'] for i in args.get('info', []) if keymatch(PMIX_GRPID, i['key'])]
    if uids == [os.geteuid()] and gids == [os.getegid()]:
        cbfunc(PMIX_SUCCESS, cbdata)
    else:
        print("ABORT2 WAS NOT GIVEN THE REQUESTER'S IDENTITY:", args.get('info'))
        cbfunc(PMIX_ERR_BAD_PARAM, cbdata)
    return PMIX_SUCCESS

def main():
    try:
        foo = PMIxServer()
    except:
        print("FAILED TO CREATE SERVER")
        exit(1)
    print("Testing server version ", foo.get_version())
    args = [{'key':'FOOBAR', 'value':'VAR', 'val_type':PMIX_STRING},
            {'key':'BLAST', 'value':7, 'val_type':PMIX_INT32}]
    map = {'clientconnected': clientconnected,
           'clientfinalized': clientfinalized,
           'fencenb': clientfence,
           'publish': clientpublish,
           'lookup': clientlookup,
           'unpublish': clientunpublish,
           'group': clientgroup,
           'abort2': clientabort2}
    my_result = foo.init(args, map)
    print("Testing PMIx_Initialized")
    rc = foo.initialized()
    print("Initialized: ", rc)
    vers = foo.get_version()
    print("Version: ", vers)

    # IOF stdin flow control. Nothing is pushing stdin to us here, so
    # these reach no producer - what they check is that the binding
    # itself is wired up: that a channel other than stdin is refused,
    # that a NULL source (None) is accepted for "everyone", and that a
    # named source and a directive list cross correctly.
    print("Testing PMIx_server_IOF_flow_control")
    rc = foo.iof_flow_control(None, PMIX_FWD_STDOUT_CHANNEL, True, [])
    if PMIX_ERR_NOT_SUPPORTED != rc:
        print("ERROR: flow control on a non-stdin channel was not refused:", rc)
        exit(1)
    for src in [None, {'nspace': "testnspace", 'rank': 0}]:
        for xoff in [True, False]:
            rc = foo.iof_flow_control(src, PMIX_FWD_STDIN_CHANNEL, xoff, [])
            if rc not in (PMIX_SUCCESS, PMIX_OPERATION_SUCCEEDED):
                print("ERROR: iof_flow_control(", src, xoff, ") failed:", rc)
                exit(1)
    print("IOF flow control: OK")

    # get our environment as a base
    env = os.environ.copy()
    # tell client.py which host it is running under: some of its checks
    # depend on the handlers this script provides
    # and keep its output if it dies or is killed
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTEST_PMIX_HOST'] = 'server'
    # register an nspace for the client app
    (rc, regex) = foo.generate_regex(["test000","test001","test002"])
    (rc, ppn) = foo.generate_ppn(["0,1,2", "3,4,5", "6,7"])
    kvals = [{'key':PMIX_NODE_MAP, 'value':regex, 'val_type':PMIX_STRING},
             {'key':PMIX_PROC_MAP, 'value':ppn, 'val_type':PMIX_STRING},
             {'key':PMIX_UNIV_SIZE, 'value':1, 'val_type':PMIX_UINT32},
             {'key':PMIX_JOB_SIZE, 'value':1, 'val_type':PMIX_UINT32}]
    print("REGISTERING NSPACE")
    rc = foo.register_nspace("testnspace", 1, kvals)
    print("RegNspace ", foo.error_string(rc))

    # exercise the current regex interface - the round trip has to give
    # back exactly the list we handed in, in the same order
    nodes = ["test000", "test001", "test002"]
    (rc, rgx) = foo.generate_regex2(nodes)
    print("Regex2: ", foo.error_string(rc), rgx)
    if PMIX_SUCCESS != rc:
        print("GENERATE_REGEX2 FAILED")
        exit(1)
    (rc, back) = foo.parse_regex2(rgx)
    print("ParseRegex2: ", foo.error_string(rc), back)
    if PMIX_SUCCESS != rc or back != nodes:
        print("REGEX2 ROUND TRIP FAILED")
        exit(1)

    # convert a cpuset string into the Python cpuset dict and back
    (rc, cpus) = foo.generate_cpuset("hwloc:0-3,8")
    print("Cpuset: ", foo.error_string(rc), cpus)
    (rc, csetstr) = foo.generate_cpuset_string(cpus)
    print("CpusetStr: ", foo.error_string(rc), csetstr)

    # collect the job info for the nspace we just registered - this is
    # what a host forwards to a remote server
    (rc, blob) = foo.collect_job_info([{'nspace': "testnspace", 'rank': 0}])
    print("CollectJobInfo: ", foo.error_string(rc), blob['size'], "bytes")
    if PMIX_SUCCESS != rc or 0 == blob['size']:
        print("COLLECT_JOB_INFO RETURNED NO DATA")
        exit(1)

    # Exercise the non-blocking form of the server registration calls.
    # PMIx spells this one as an optional callback on the same entry
    # point rather than as a separate _nb function, and it is the only
    # form usable from inside a server module upcall - those run on the
    # progress thread, where the blocking form would be waiting on the
    # event loop it is standing in.
    print("Testing the non-blocking server registration calls")
    nbdone = threading.Event()
    nbresult = {}

    def nbcb(status, cbdata):
        # runs on the PMIx progress thread
        nbresult['status'] = status
        nbresult['cbdata'] = cbdata
        nbdone.set()

    def drive(label, rc):
        # a callback fires if and only if the request was accepted
        if PMIX_SUCCESS != rc:
            print("%s NOT ACCEPTED: %s" % (label, foo.error_string(rc)))
            exit(1)
        if not nbdone.wait(timeout=30):
            print("%s CALLBACK NEVER FIRED" % label)
            exit(1)
        if PMIX_SUCCESS != nbresult['status']:
            print("%s COMPLETED WITH %s" % (label,
                                            foo.error_string(nbresult['status'])))
            exit(1)
        if nbresult['cbdata'] != {'label': label}:
            print("%s RETURNED THE WRONG CBDATA: %s" % (label, nbresult['cbdata']))
            exit(1)
        print("  %s: completed asynchronously" % label)
        nbdone.clear()

    drive("register_nspace", foo.register_nspace("nbnspace", 1, kvals,
                                                 nbcb, {'label': "register_nspace"}))
    drive("register_client", foo.register_client({'nspace': "nbnspace", 'rank': 0},
                                                 os.geteuid(), os.getegid(),
                                                 nbcb, {'label': "register_client"}))
    drive("deregister_client", foo.deregister_client({'nspace': "nbnspace", 'rank': 0},
                                                     nbcb, {'label': "deregister_client"}))
    drive("deregister_nspace", foo.deregister_nspace("nbnspace", nbcb,
                                                     {'label': "deregister_nspace"}))
    # The methods that wait in Python for a completion the progress
    # thread delivers cannot be called on that thread: nothing could
    # deliver it. They used to wait there, and stopped the server for
    # good; they have to refuse, as the library's own blocking calls do
    wbgot = {}

    def wbcb(status, cbdata):
        # runs on the PMIx progress thread
        wbgot['setup_application'] = foo.setup_application("wbnspace", [])[0]
        wbgot['dmodex_request'] = foo.dmodex_request({'nspace': "wbnspace", 'rank': 0}, {})[0]
        wbgot['dmodex_request2'] = foo.dmodex_request2({'nspace': "wbnspace", 'rank': 0}, [])[0]
        wbgot['collect_inventory'] = foo.collect_inventory([])[0]
        nbcb(status, cbdata)

    rc = foo.register_nspace("wbnspace", 1, kvals, wbcb, {'label': "register_nspace"})
    drive("register_nspace", rc)
    for (method, got) in sorted(wbgot.items()):
        if PMIX_ERR_WOULD_BLOCK != got:
            print("%s FROM THE PROGRESS THREAD ANSWERED %s" % (method, foo.error_string(got)))
            exit(1)
    if 4 != len(wbgot):
        print("NOT EVERY WAITING METHOD WAS TRIED:", wbgot)
        exit(1)
    drive("deregister_nspace", foo.deregister_nspace("wbnspace", nbcb,
                                                     {'label': "deregister_nspace"}))
    print("  the waiting methods refuse on the progress thread")
    # a session is registered and deregistered explicitly, in either form
    sinfo = [{'key': PMIX_UNIV_SIZE, 'value': 4, 'val_type': PMIX_UINT32}]
    drive("register_session", foo.register_session(77, sinfo, nbcb,
                                                   {'label': "register_session"}))
    drive("deregister_session", foo.deregister_session(77, nbcb,
                                                       {'label': "deregister_session"}))
    rc = foo.register_session(78, sinfo)
    if PMIX_SUCCESS != rc:
        print("BLOCKING register_session FAILED:", foo.error_string(rc))
        exit(1)
    if PMIX_SUCCESS != foo.deregister_session(78):
        print("BLOCKING deregister_session FAILED")
        exit(1)
    # UINT32_MAX is the library's "no session", so it is refused
    rc = foo.register_session(4294967295, sinfo)
    if PMIX_ERR_BAD_PARAM != rc:
        print("register_session ACCEPTED UINT32_MAX:", foo.error_string(rc))
        exit(1)
    print("  register_session/deregister_session: both forms OK")
    # a None source means "this process", as a NULL does in C
    rc = foo.notify_event(PMIX_ERR_JOB_TERMINATED, None, PMIX_RANGE_LOCAL, [])
    if PMIX_SUCCESS != rc:
        print("notify_event WITH NO SOURCE FAILED:", foo.error_string(rc))
        exit(1)
    # the rest of the optional-callback family, against a live library.
    # Several of these legitimately answer NOT_SUPPORTED here - there is
    # no host above us to service them - and what matters is the contract:
    # a callback fires if and only if the call reported PMIX_SUCCESS.
    def drive_opt(label, rc):
        if PMIX_SUCCESS == rc:
            if not nbdone.wait(timeout=30):
                print("%s ACCEPTED BUT NEVER COMPLETED" % label)
                exit(1)
            if nbresult['cbdata'] != {'label': label}:
                print("%s RETURNED THE WRONG CBDATA" % label)
                exit(1)
            print("  %s: completed asynchronously (%s)"
                  % (label, foo.error_string(nbresult['status'])))
            nbdone.clear()
        else:
            # refused up front, so no callback may fire
            if nbdone.is_set():
                print("%s FIRED A CALLBACK IT DID NOT PROMISE" % label)
                exit(1)
            print("  %s: refused up front (%s)" % (label, foo.error_string(rc)))

    src = {'nspace': "testnspace", 'rank': 0}
    payload = {'bytes': b"async iof payload\n", 'size': 18}
    drive_opt("register_resources",
              foo.register_resources([], nbcb, {'label': "register_resources"}))
    drive_opt("deregister_resources",
              foo.deregister_resources([], nbcb, {'label': "deregister_resources"}))
    drive_opt("setup_local_support",
              foo.setup_local_support("testnspace", [], nbcb,
                                      {'label': "setup_local_support"}))
    drive_opt("deliver_inventory",
              foo.deliver_inventory([], [], nbcb, {'label': "deliver_inventory"}))
    drive_opt("iof_deliver",
              foo.iof_deliver(src, PMIX_FWD_STDOUT_CHANNEL, payload, [], nbcb,
                              {'label': "iof_deliver"}))
    drive_opt("iof_flow_control",
              foo.iof_flow_control(None, PMIX_FWD_STDIN_CHANNEL, True, [], nbcb,
                                   {'label': "iof_flow_control"}))
    # nothing is collecting, so there is nothing to expire - the callback
    # reports that
    drive_opt("collective_expire",
              foo.collective_expire(PMIX_COLLECTIVE_GROUP_CONSTRUCT, "nogroup", None, [],
                                    nbcb, {'label': "collective_expire"}))
    rc = foo.collective_expire(PMIX_COLLECTIVE_FENCE, None, [src], [])
    if PMIX_ERR_NOT_FOUND != rc:
        print("collective_expire FOUND A FENCE NOBODY CALLED:", foo.error_string(rc))
        exit(1)
    print("  collective_expire: blocking form answered (%s)" % foo.error_string(rc))
    # session_control is the one member of this family that reports
    # results, so it takes the info-style callback - three arguments, not
    # two - and needs its own waiter.  A standalone server has no
    # scheduler above it to service the request, so the library answers
    # PMIX_ERR_UNREACH through the callback; what is being checked here is
    # that the callback is reached at all, and with the caller's cbdata.
    sesdone = threading.Event()
    sesresult = {}

    def sescb(status, results, cbdata):
        sesresult['status'] = status
        sesresult['results'] = results
        sesresult['cbdata'] = cbdata
        sesdone.set()

    rc = foo.session_control(1, [], sescb, {'label': "session_control"})
    if PMIX_SUCCESS != rc:
        print("session_control NOT ACCEPTED:", foo.error_string(rc))
        exit(1)
    if not sesdone.wait(timeout=30):
        print("session_control ACCEPTED BUT NEVER COMPLETED")
        exit(1)
    if sesresult['cbdata'] != {'label': "session_control"}:
        print("session_control RETURNED THE WRONG CBDATA")
        exit(1)
    if not isinstance(sesresult['results'], list):
        print("session_control DID NOT HAND BACK A RESULTS LIST")
        exit(1)
    print("  session_control: completed asynchronously (%s)"
          % foo.error_string(sesresult['status']))

    # a bad callback is refused before anything is allocated, on every one
    # of them
    for label, rc in [
            ("register_nspace", foo.register_nspace("nbnspace", 1, [], 42)),
            ("deregister_nspace", foo.deregister_nspace("nbnspace", 42)),
            ("register_client", foo.register_client(src, 0, 0, 42)),
            ("deregister_client", foo.deregister_client(src, 42)),
            ("notify_event", foo.notify_event(PMIX_ERR_JOB_TERMINATED, src,
                                              PMIX_RANGE_LOCAL, [], 42)),
            ("register_resources", foo.register_resources([], 42)),
            ("deregister_resources", foo.deregister_resources([], 42)),
            ("setup_local_support", foo.setup_local_support("n", [], 42)),
            ("deliver_inventory", foo.deliver_inventory([], [], 42)),
            ("iof_deliver", foo.iof_deliver(src, PMIX_FWD_STDOUT_CHANNEL,
                                            payload, [], 42)),
            ("iof_flow_control", foo.iof_flow_control(None,
                                                      PMIX_FWD_STDIN_CHANNEL,
                                                      True, [], 42)),
            ("session_control", foo.session_control(1, [], 42)),
            ("register_session", foo.register_session(1, [], 42)),
            ("deregister_session", foo.deregister_session(1, 42)),
            ("collective_expire", foo.collective_expire(PMIX_COLLECTIVE_GROUP_CONSTRUCT, "g",
                                                        None, [], 42))]:
        if PMIX_ERR_BAD_PARAM != rc:
            print("%s ACCEPTED A NON-CALLABLE CALLBACK" % label)
            exit(1)
    print("Non-blocking server registration calls: OK")

    # print a value and an info the way the library itself renders them
    (rc, txt) = foo.data_print("VALUE: ", {'value': 42, 'val_type': PMIX_INT32})
    print("DataPrint: ", foo.error_string(rc), txt)

    # render each of the structs the library knows how to print
    aval = {'value': 42, 'val_type': PMIX_INT32}
    ainfo = {'key': PMIX_JOB_SIZE, 'value': 4, 'val_type': PMIX_UINT32}
    aproc = {'nspace': "testnspace", 'rank': 0}
    anapp = {'cmd': "/bin/true", 'argv': ["true"], 'maxprocs': 1, 'info': []}
    aunit = {'type': PMIX_DEVTYPE_GPU, 'count': 2}
    for (label, result) in [("ValueString", foo.value_string(aval)),
                            ("InfoString", foo.info_string(ainfo)),
                            ("ProcString", foo.proc_string(aproc)),
                            ("AppString", foo.app_string(anapp)),
                            ("UnitString", foo.resource_unit_string(aunit))]:
        (rc, txt) = result
        print("%s: %s %s" % (label, foo.error_string(rc), repr(txt)))
        if PMIX_SUCCESS != rc or txt is None:
            print("%s FAILED" % label)
            exit(1)

    # exercise the serialization family. A Python data buffer is just a
    # dict, so the round trip below also proves that a buffer can be
    # carried as ordinary bytes and picked up again
    sval = {'value': "hello world", 'val_type': PMIX_STRING}
    (rc, buf) = foo.data_pack(None, aval)
    if PMIX_SUCCESS == rc:
        (rc, buf) = foo.data_pack(buf, sval)
    if PMIX_SUCCESS == rc:
        (rc, buf) = foo.data_pack(buf, ainfo)
    print("DataPack: ", foo.error_string(rc), buf)
    if PMIX_SUCCESS != rc:
        print("DATA PACK FAILED")
        exit(1)

    # a fresh buffer built from nothing but the payload bytes must unpack
    # to exactly what went in, in order
    wire = {'bytes': buf['bytes'], 'bytes_used': buf['bytes_used'],
            'bytes_unpacked': 0}
    (rc, v1) = foo.data_unpack(wire, PMIX_VALUE)
    (rc2, v2) = foo.data_unpack(wire, PMIX_VALUE)
    (rc3, v3) = foo.data_unpack(wire, PMIX_INFO)
    print("DataUnpack: ", foo.error_string(rc), v1, v2, v3)
    if PMIX_SUCCESS != rc or PMIX_SUCCESS != rc2 or PMIX_SUCCESS != rc3:
        print("DATA UNPACK FAILED")
        exit(1)
    # the constants are bytes while an unpacked key comes back as str
    wantkey = ainfo['key']
    if isinstance(wantkey, bytes):
        wantkey = wantkey.decode('ascii')
    if v1['value'] != aval['value'] or v2['value'] != sval['value'] \
       or v3['key'] != wantkey or v3['value'] != ainfo['value']:
        print("DATA UNPACK ROUND TRIP MISMATCH")
        exit(1)
    # and the buffer is now drained
    (rc, extra) = foo.data_unpack(wire, PMIX_VALUE)
    if PMIX_SUCCESS == rc:
        print("DATA UNPACK RETURNED DATA PAST THE END")
        exit(1)

    # A regex is a C string to everything in the library that reads it,
    # so it has to be stored with its NUL: one loaded without it was read
    # past the end of its allocation when packed, and came back with a NUL
    # that had never been part of it. And a value with no topology or
    # cpuset is how a caller says "none"; it packs as exactly that
    for v in ({'value': b'pmix[n1,n2]', 'val_type': PMIX_REGEX},
              {'value': None, 'val_type': PMIX_TOPO},
              {'value': None, 'val_type': PMIX_PROC_CPUSET}):
        (rc, rbuf) = foo.data_pack(None, v)
        if PMIX_SUCCESS != rc:
            print("DATA PACK OF %s FAILED: %s" % (v, foo.error_string(rc)))
            exit(1)
        (rc, back) = foo.data_unpack(rbuf, PMIX_VALUE)
        if PMIX_SUCCESS != rc or back != v:
            print("DATA ROUND TRIP OF %s CAME BACK AS %s (%s)" % (v, back, foo.error_string(rc)))
            exit(1)
    print("DataPack: regex, topology and cpuset values round trip")

    # unload the payload out of a buffer and load it into another
    (rc, buf2) = foo.data_pack(None, aval)
    (rc, payload) = foo.data_unload(buf2)
    print("DataUnload: ", foo.error_string(rc), payload, "buffer now", buf2)
    if PMIX_SUCCESS != rc or 0 == payload['size']:
        print("DATA UNLOAD FAILED")
        exit(1)
    (rc, buf3) = foo.data_load(None, payload)
    (rc2, back) = foo.data_unpack(buf3, PMIX_VALUE)
    print("DataLoad: ", foo.error_string(rc), back)
    if PMIX_SUCCESS != rc or PMIX_SUCCESS != rc2 or back['value'] != aval['value']:
        print("DATA LOAD ROUND TRIP FAILED")
        exit(1)

    # every byte value has to survive the crossing, not just ASCII
    blob = bytes(range(256))
    (rc, comp) = foo.data_compress(blob * 32)
    print("DataCompress: ", foo.error_string(rc),
          None if comp is None else len(comp))
    if PMIX_SUCCESS == rc:
        (rc, back) = foo.data_decompress(comp)
        if PMIX_SUCCESS != rc or bytes(back) != blob * 32:
            print("COMPRESSION ROUND TRIP FAILED")
            exit(1)
        print("DataDecompress: round trip verified")

    # register a client
    uid = os.getuid()
    gid = os.getgid()
    rc = foo.register_client({'nspace':"testnspace", 'rank':0}, uid, gid)
    print("RegClient ", foo.error_string(rc))
    # setup the fork
    rc = foo.setup_fork({'nspace':"testnspace", 'rank':0}, env)
    print("SetupFrk", foo.error_string(rc))

    # setup the client argv.  Locate the client beside this script
    # rather than in cwd: under a VPATH build we are run from the build
    # directory, which holds no .py files.
    # Start it with this interpreter rather than through its
    # "#!/usr/bin/env python3" line. On macOS /usr/bin/env is a
    # protected binary, and System Integrity Protection strips
    # DYLD_LIBRARY_PATH when it runs one - so a client started that way
    # cannot find the libpmix in an uninstalled build tree, and fails
    # at import. run_server.sh names the interpreter for the same reason.
    args = [sys.executable,
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "client.py")]
    # open a subprocess with stdout and stderr
    # as distinct pipes so we can capture their
    # output as the process runs
    p = subprocess.Popen(args, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Drain the child's pipes until both reach EOF, then reap it - the
    # same loop as sched.py, which explains it. The one that was here had
    # no bound on a wedged child, could stop on one pipe's EOF while the
    # other still held output, and never looked at how the child exited,
    # so a client that FAILED passed this test.
    idle_limit = 60
    deadline = time.time() + idle_limit
    reads = [p.stdout, p.stderr]
    while reads:
        ready, _, _ = select.select(reads, [], [], 1.0)
        if not ready:
            if time.time() > deadline:
                print("CLIENT PRODUCED NO OUTPUT FOR %d SECONDS - KILLING IT"
                      % idle_limit)
                p.kill()
                break
            continue
        for f in ready:
            line = f.readline()
            if not line:
                # EOF on this pipe - stop watching it, keep the other
                reads.remove(f)
                continue
            label = 'stdout: ' if f is p.stdout else 'stderr: '
            print(label + line.decode('utf-8').rstrip())
            deadline = time.time() + idle_limit

    status = p.wait()
    print("CLIENT EXITED WITH", status)

    print("FINALIZING")
    foo.finalize()

    if 0 != status:
        exit(1)


if __name__ == '__main__':
    global killer
    killer = GracefulKiller()
    main()
