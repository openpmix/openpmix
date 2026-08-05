#!/usr/bin/env python3

from pmix import *
import signal, time
import os
import select
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
    for d in args['directives']:
        if 'pmix' in d['key']:
            # a directive qualifying the request, not data to publish
            continue
        published[normalize_key(d['key'])] = {'proc': args['proc'],
                                              'value': d['value'],
                                              'val_type': d['val_type']}
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

def clientlookup(args, cbfunc, cbdata):
    print("LOOKUP", args['proc'], args['keys'])
    pdata = []
    for ky in args['keys']:
        ky = normalize_key(ky)
        if ky in published:
            d = published[ky]
            pdata.append({'proc': d['proc'], 'key': ky,
                          'value': d['value'], 'val_type': d['val_type']})
    if 0 == len(pdata):
        cbfunc(PMIX_ERR_NOT_FOUND, None, cbdata)
    else:
        cbfunc(PMIX_SUCCESS, pdata, cbdata)
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
    # no data to return from this (empty) fence
    cbfunc(PMIX_SUCCESS, bytearray(0), cbdata)
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
           'group': clientgroup}
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
    # a bad callback is refused before anything is allocated
    if PMIX_ERR_BAD_PARAM != foo.deregister_nspace("nbnspace", "not callable"):
        print("A NON-CALLABLE CALLBACK WAS ACCEPTED")
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

    # setup the client argv
    args = ["./client.py"]
    # open a subprocess with stdout and stderr
    # as distinct pipes so we can capture their
    # output as the process runs
    p = subprocess.Popen(args, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # define storage to catch the output
    stdout = []
    stderr = []
    # loop until the pipes close
    while True:
        reads = [p.stdout.fileno(), p.stderr.fileno()]
        ret = select.select(reads, [], [])

        stdout_done = True
        stderr_done = True

        for fd in ret[0]:
            # if the data
            if fd == p.stdout.fileno():
                read = p.stdout.readline()
                if read:
                    read = read.decode('utf-8').rstrip()
                    print('stdout: ' + read)
                    stdout_done = False
            elif fd == p.stderr.fileno():
                read = p.stderr.readline()
                if read:
                    read = read.decode('utf-8').rstrip()
                    print('stderr: ' + read)
                    stderr_done = False

        if stdout_done and stderr_done:
            break
    print("FINALIZING")
    foo.finalize()


if __name__ == '__main__':
    global killer
    killer = GracefulKiller()
    main()
