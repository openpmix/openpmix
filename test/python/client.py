#!/usr/bin/env python3

from pmix import *
import pmix
import faulthandler
import os
import time
import threading

termEvent = threading.Event()
grpConstructed = threading.Event()
grpDestructed = threading.Event()
grpResults = []
grpStatus = PMIX_ERR_NOT_SUPPORTED

def default_evhandler(evhdlr:int, status:int,
                      source:dict, info:list, results:list):
    print("DEFAULT HANDLER")
    return PMIX_EVENT_ACTION_COMPLETE,None

# Generic callbacks for the non-blocking client operations. Like the group
# callbacks below they run on the PMIx progress thread, so they only record
# what came back and release the waiter.
class nbwaiter:
    """Collects the result of one non-blocking operation."""

    def __init__(self, label):
        self.label = label
        self.event = threading.Event()
        self.status = PMIX_ERR_NOT_SUPPORTED
        self.results = None

    def op_cb(self, status:int, cbdata):
        print(self.label, "CALLBACK", status)
        self.status = status
        self.event.set()

    def data_cb(self, status:int, results, cbdata):
        print(self.label, "CALLBACK", status, results)
        self.status = status
        self.results = results
        self.event.set()

    def cred_cb(self, status:int, credential:dict, results:list, cbdata):
        print(self.label, "CALLBACK", status, credential)
        self.status = status
        self.results = credential
        self.event.set()

    def wait(self, client, rc):
        # a callback fires if and only if the request was accepted
        if PMIX_SUCCESS != rc:
            print(self.label, "not accepted:", client.error_string(rc))
            return False
        if not self.event.wait(timeout=30):
            print(self.label, "TIMED OUT")
            return False
        print(self.label, "completed:", client.error_string(self.status))
        return PMIX_SUCCESS == self.status

# Callbacks for the non-blocking group operations. These are executed on
# the PMIx progress thread, so they do nothing but record the result and
# release the waiter - a blocking PMIx call from here would deadlock.
def group_construct_cb(status:int, results:list, cbdata):
    global grpResults, grpStatus
    print("GROUP CONSTRUCT CALLBACK", status)
    grpStatus = status
    grpResults = results
    cbdata.set()

def group_destruct_cb(status:int, cbdata):
    print("GROUP DESTRUCT CALLBACK", status)
    cbdata.set()

def model_evhandler(evhdlr:int, status:int,
                    source:dict, info:list, results:list):
    global termEvent

    print("MODEL HANDLER")
    termEvent.set()
    return PMIX_EVENT_ACTION_COMPLETE,None

# Checks whose failure fails this client, collected so that it still
# finalizes cleanly first
failures = []

def check(ok, what):
    if ok:
        print("CHECK PASS:", what)
    else:
        print("CHECK FAIL:", what)
        failures.append(what)

# The progress thread must be able to run Python while this process's
# main thread waits in a blocking call. This handler takes the GIL on the
# progress thread and then gives it up while it sleeps, so the main
# thread is free to enter put() - and the handler cannot finish until it
# has the GIL back. A blocking call that held the GIL while it waited for
# the progress thread would deadlock against it here.
gilStarted = threading.Event()
gilDone = threading.Event()
GIL_EVENT = PMIX_EXTERNAL_ERR_BASE - 11

gilTimes = {}

def gil_evhandler(evhdlr:int, status:int,
                  source:dict, info:list, results:list):
    gilStarted.set()
    time.sleep(0.5)
    gilTimes['done'] = time.monotonic()
    gilDone.set()
    return PMIX_EVENT_ACTION_COMPLETE, None

# A blocking registration made from a handler, on the progress thread,
# cannot complete. It has to say so at once - not wait on the lock a
# registration on another thread may be holding while it waits for this
# very thread
REG_EVENT = PMIX_EXTERNAL_ERR_BASE - 13
regSeen = threading.Event()
regGot = {}

def reg_evhandler(evhdlr:int, status:int,
                  source:dict, info:list, results:list):
    regGot['rc'] = regclient.register_event_handler([REG_EVENT - 1], None,
                                                    default_evhandler)
    regSeen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None

regclient = None

# The source a non-blocking notification names has to survive the call:
# the event is delivered after notify_event has returned
SRC_EVENT = PMIX_EXTERNAL_ERR_BASE - 12
srcSeen = threading.Event()
srcGot = {}

def src_evhandler(evhdlr:int, status:int,
                  source:dict, info:list, results:list):
    srcGot['source'] = source
    srcSeen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None

# A handler's status goes back to the library as a C status. One that does
# not fit in one used to raise there, outside the handler's protection,
# and the event was never completed: the handlers after it in the chain
# never ran
CHAIN_EVENT = PMIX_EXTERNAL_ERR_BASE - 14
chainSeen = threading.Event()

def chain_bad_evhandler(evhdlr:int, status:int,
                        source:dict, info:list, results:list):
    return 2 ** 40, None

def chain_next_evhandler(evhdlr:int, status:int,
                         source:dict, info:list, results:list):
    chainSeen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None

def main():
    global termEvent
    host = os.environ.get('PYTEST_PMIX_HOST')
    
    foo = PMIxClient()
    print("Testing PMIx ", foo.get_version())
    info = [{'key':PMIX_PROGRAMMING_MODEL, 'value':'TEST', 'val_type':PMIX_STRING},
            {'key':PMIX_MODEL_LIBRARY_NAME, 'value':'PMIX', 'val_type':PMIX_STRING}]
    my_result = foo.init(info)
    print("Init result ", my_result)
    if 0 != my_result[0]:
        print("FAILED TO INIT")
        exit(1)
    # init took who we are out of the environment, and the bindings out of
    # os.environ too - the environment a process Python starts is given
    left = [k for k in os.environ
            if k in ('PMIX_NAMESPACE', 'PMIX_RANK', 'PMIX_CONNECT_TOKEN',
                     'PMIX_SERVER_LOCAL_URI', 'PMIX_SECURITY_MODE', 'PMIX_VERSION')
            or k.startswith('PMIX_SERVER_URI')]
    if left:
        print("STILL IN os.environ:", left)
        exit(1)
    # register an event handler
    rc,myevhndlr = foo.register_event_handler(None, None, default_evhandler)
    print("REGISTER DEFAULT", foo.error_string(rc))
    # register the model handler
    rc,mymodelhndlr = foo.register_event_handler([PMIX_MODEL_DECLARED], None, model_evhandler)
    print("REGISTER MODEL", foo.error_string(rc))
    # The model event was raised inside init, before this handler existed,
    # so the library replays it from its cache as part of the registration.
    # The handler must have it by the time registration returns - not from
    # a retry some time later, which could find no handler and drop it.
    replayed = termEvent.is_set()
    print("MODEL EVENT DELIVERED DURING REGISTRATION:", replayed)

    # try putting something
    print("PUT")
    rc = foo.put(PMIX_GLOBAL, "mykey", {'value':1, 'val_type':PMIX_INT32})
    print("Put result ",foo.error_string(rc));
    # commit it
    print("COMMIT")
    rc = foo.commit()
    print ("Commit result ", foo.error_string(rc))
    # a blocking call made while a Python event handler is mid-flight on
    # the progress thread. A watchdog turns a deadlock into a failure
    print("GIL")
    rc, gilhdlr = foo.register_event_handler([GIL_EVENT], None, gil_evhandler)
    check(0 <= rc or PMIX_SUCCESS == rc, "registered the GIL handler")
    # The watchdog cannot be a Python timer: a deadlock here is one in
    # which the main thread holds the GIL, so no Python thread could run
    # to notice it. faulthandler's watchdog is a C thread that never
    # takes the GIL; it prints every thread's stack and exits
    print("If this hangs, a blocking call is holding the GIL while it "
          "waits for the progress thread", flush=True)
    faulthandler.dump_traceback_later(20, exit=True)
    w = nbwaiter("GIL NOTIFY")
    rc = foo.notify_event(GIL_EVENT, my_result[1], PMIX_RANGE_PROC_LOCAL, [],
                          w.op_cb, "gil")
    check(PMIX_SUCCESS == rc, "the notification is accepted")
    check(gilStarted.wait(timeout=10), "the handler started")
    gilTimes['put'] = time.monotonic()
    rc = foo.put(PMIX_GLOBAL, "gilkey", {'value': 2, 'val_type': PMIX_INT32})
    check(PMIX_SUCCESS == rc, "put while a handler waits for the GIL")
    rc = foo.commit()
    check(PMIX_SUCCESS == rc, "commit while a handler waits for the GIL")
    check(gilDone.wait(timeout=10), "the handler finished")
    # the case is only a case if the two overlapped
    check(gilTimes.get('put', 0) < gilTimes.get('done', 0),
          "put() began while the handler still held its event")
    faulthandler.cancel_dump_traceback_later()
    foo.deregister_event_handler(gilhdlr)

    print("REGISTER FROM A HANDLER")
    global regclient
    regclient = foo
    rc, reghdlr = foo.register_event_handler([REG_EVENT], None, reg_evhandler)
    w = nbwaiter("REG NOTIFY")
    # hold the registration lock, as a registration on another thread
    # would while it waits for the progress thread: the handler's own
    # attempt must not wait for it
    with pmix._reglock:
        rc = foo.notify_event(REG_EVENT, my_result[1], PMIX_RANGE_PROC_LOCAL, [],
                              w.op_cb, "reg")
        seen = regSeen.wait(timeout=10)
    check(seen and (PMIX_ERR_WOULD_BLOCK, -1) == regGot.get('rc'),
          "a blocking registration from a handler is refused at once: %r"
          % (regGot.get('rc'),))
    foo.deregister_event_handler(reghdlr)

    print("NOTIFY SOURCE")
    rc, srchdlr = foo.register_event_handler([SRC_EVENT], None, src_evhandler)
    w = nbwaiter("NOTIFY SOURCE")
    rc = foo.notify_event(SRC_EVENT, {'nspace': "pytest-source", 'rank': 7},
                          PMIX_RANGE_PROC_LOCAL, [], w.op_cb, "src")
    check(PMIX_SUCCESS == rc, "a non-blocking notification naming a source is accepted")
    check(srcSeen.wait(timeout=10) and
          {'nspace': "pytest-source", 'rank': 7} == srcGot.get('source'),
          "the handler sees the source the notification named: %r"
          % (srcGot.get('source'),))
    foo.deregister_event_handler(srchdlr)

    print("EVENT CHAIN")
    rc, nexthdlr = foo.register_event_handler([CHAIN_EVENT], None, chain_next_evhandler)
    rc, badhdlr = foo.register_event_handler([CHAIN_EVENT],
                                             [{'key': PMIX_EVENT_HDLR_PREPEND,
                                               'value': True, 'val_type': PMIX_BOOL}],
                                             chain_bad_evhandler)
    w = nbwaiter("EVENT CHAIN")
    rc = foo.notify_event(CHAIN_EVENT, my_result[1], PMIX_RANGE_PROC_LOCAL, [],
                          w.op_cb, "chain")
    check(PMIX_SUCCESS == rc and chainSeen.wait(timeout=10),
          "a handler answering a status out of range does not stop the chain")
    foo.deregister_event_handler(badhdlr)
    foo.deregister_event_handler(nexthdlr)

    # execute fence
    print("FENCE")
    procs = []
    info = []
    rc = foo.fence(procs, info)
    print("Fence result ", foo.error_string(rc))
    print("GET")
    info = []
    rc, get_val = foo.get({'nspace':"testnspace", 'rank': 0}, "mykey", info)
    print("Get result: ", foo.error_string(rc))
    print("Get value returned: ", get_val)
    # send a heartbeat to our server - this is the fire-and-forget path
    # that feeds a process health monitor, so there is nothing to wait for
    print("HEARTBEAT")
    rc = foo.heartbeat()
    print("Heartbeat result ", foo.error_string(rc))
    # ask the scheduler to define a resource block. Our server is not a
    # scheduler and has no scheduler above it, so the library has no one
    # to ask - what is under test here is that the request converts and
    # comes back with a status rather than hanging or crashing
    print("RESOURCE BLOCK")
    units = [{'type': PMIX_DEVTYPE_GPU, 'count': 2}]
    rc = foo.resource_block(PMIX_RESOURCE_BLOCK_DEFINE, "myblock", units, [])
    print("Resource block result ", foo.error_string(rc))
    if 'scheduler' == host:
        # our server is the scheduler, and its handler checks what arrives
        check(PMIX_SUCCESS == rc, "the scheduler defines a resource block")
        rc = foo.resource_block(PMIX_RESOURCE_BLOCK_DELETE, "myblock", [], [])
        check(PMIX_SUCCESS == rc,
              "the scheduler deletes a resource block, which carries no units: %s"
              % foo.error_string(rc))
    # ask for a process that does not exist to be aborted - our server
    # checks that the request names us, and refuses it otherwise
    print("ABORT")
    rc = foo.abort(PMIX_ERR_JOB_ABORTED, "client abort test",
                   [{'nspace': "testnspace", 'rank': 99}])
    print("Abort result ", foo.error_string(rc))
    if PMIX_SUCCESS != rc:
        print("ABORT WAS NOT ACCEPTED")
        exit(1)
    # test a fence that should return not_supported because
    # we pass a required attribute that doesn't exist
    procs = []
    info = [{'key': 'ARBITRARY', 'flags': PMIX_INFO_REQD, 'value':10, 'val_type':PMIX_INT}]
    rc = foo.fence(procs, info)
    print("Fence should be not supported:", foo.error_string(rc))

    # now exercise the non-blocking forms of the operations we just ran
    # blocking. Each of these hands its converted arguments to the library
    # and gets the result back on the progress thread.
    print("FENCE NB")
    w = nbwaiter("FENCE NB")
    rc = foo.fence_nb(None, [], w.op_cb, "fence")
    w.wait(foo, rc)

    print("GET NB")
    w = nbwaiter("GET NB")
    rc = foo.get_nb({'nspace':"testnspace", 'rank': 0}, "mykey", [],
                    w.data_cb, "get")
    if w.wait(foo, rc):
        print("Get_nb value returned: ", w.results)

    # the value a get_nb callback is handed is the receiver's to release.
    # The bindings converted it and dropped it, so every call leaked a
    # copy of the value: 2000 gets of 64 KiB grew the process by about
    # 128 MiB. A peak-RSS check with a wide margin catches that
    import resource, sys
    def peak_mib():
        r = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return r / (1024 * 1024) if sys.platform == 'darwin' else r / 1024
    big = "x" * 65536
    rc = foo.put(PMIX_GLOBAL, "pybigkey", {'value': big, 'val_type': PMIX_STRING})
    if PMIX_SUCCESS == rc:
        rc = foo.commit()
    leakev = threading.Event()
    leakseen = []
    def leakcb(status, value, cbdata):
        leakseen.append(PMIX_SUCCESS == status and value is not None and
                        len(value['value']) == len(big))
        leakev.set()
    me = my_result[1]
    before = None
    for n in range(2200):
        if 200 == n:
            before = peak_mib()
        leakev.clear()
        rc = foo.get_nb(me, "pybigkey", [], leakcb, None)
        if PMIX_SUCCESS != rc or not leakev.wait(30):
            break
    grown = peak_mib() - before if before is not None else None
    check(2200 == len(leakseen) and all(leakseen),
          "get_nb answers a large value every time")
    # AddressSanitizer holds freed memory in a quarantine (256 MiB by
    # default) rather than reusing it, so under it the peak grows whether
    # or not the value is released - and ASan's own leak checker is the
    # better test there anyway
    def asan_loaded():
        try:
            with open("/proc/self/maps") as f:
                return "libasan" in f.read()
        except OSError:
            return "asan" in os.environ.get("DYLD_INSERT_LIBRARIES", "")
    if asan_loaded():
        print("SKIP: peak-RSS check under AddressSanitizer (%s MiB)" % grown)
    else:
        check(grown is not None and grown < 32,
              "repeated get_nb does not grow the process (%s MiB)" % grown)

    print("PUBLISH NB")
    w = nbwaiter("PUBLISH NB")
    pdata = [{'key':'pykey', 'value':'pyvalue', 'val_type':PMIX_STRING}]
    rc = foo.publish_nb(pdata, w.op_cb, "publish")
    if w.wait(foo, rc):
        print("LOOKUP NB")
        w = nbwaiter("LOOKUP NB")
        rc = foo.lookup_nb(['pykey'], [], w.data_cb, "lookup")
        if w.wait(foo, rc):
            print("Lookup_nb returned: ", w.results)
        print("UNPUBLISH NB")
        w = nbwaiter("UNPUBLISH NB")
        rc = foo.unpublish_nb(['pykey'], [], w.op_cb, "unpublish")
        w.wait(foo, rc)

    if 'server' == host:
        # a lookup that finds only some of its keys reports the ones it
        # found, and the rest as undefined
        rc = foo.publish([{'key': 'pyfound', 'value': 'here', 'val_type': PMIX_STRING}])
        check(PMIX_SUCCESS == rc, "publish pyfound")
        rc, res = foo.lookup([{'key': 'pyfound'}, {'key': 'pymissing'}], None)
        found = [r for r in res if 'pyfound' == r['key']]
        missing = [r for r in res if 'pymissing' == r['key']]
        check(rc in (PMIX_SUCCESS, PMIX_ERR_PARTIAL_SUCCESS) and
              1 == len(found) and 'here' == found[0]['value'] and
              (0 == len(missing) or PMIX_UNDEF == missing[0]['val_type']),
              "a partial lookup reports what it found: %s %r"
              % (foo.error_string(rc), res))
        # a handler that completes and then returns no status
        rc = foo.publish([{'key': 'pytest.noreturn', 'value': 1, 'val_type': PMIX_INT32}])
        check(PMIX_SUCCESS == rc,
              "a handler that completed and returned nothing is taken at its word: %s"
              % foo.error_string(rc))
        # a handler that completes and then raises
        rc, res = foo.lookup([{'key': 'pyfound'}, {'key': 'pytest.raiseafter'}], None)
        check(rc in (PMIX_SUCCESS, PMIX_ERR_PARTIAL_SUCCESS) and
              any('pyfound' == r['key'] and 'here' == r['value'] for r in res),
              "a handler that completed and then raised is taken at its word: %s"
              % foo.error_string(rc))
        # a helper handed an answer it cannot even measure
        w = nbwaiter("LOOKUP GENERATOR")
        rc = foo.lookup_nb(['pytest.generator'], [], w.data_cb, "generator")
        check(PMIX_SUCCESS == rc and w.event.wait(timeout=30) and
              PMIX_SUCCESS != w.status,
              "an answer the helper cannot measure completes with an error")
        # a completion given something that is not a status
        w = nbwaiter("PUBLISH BADSTATUS")
        rc = foo.publish_nb([{'key': 'pytest.badstatus', 'value': 1,
                              'val_type': PMIX_INT32}], w.op_cb, "badstatus")
        check(PMIX_SUCCESS == rc and w.event.wait(timeout=30) and
              PMIX_SUCCESS != w.status,
              "a completion given a non-status completes with an error")
        # a handler whose answer the bindings cannot convert: the request
        # is still completed, with an error
        w = nbwaiter("LOOKUP BADVALUE")
        rc = foo.lookup_nb(['pytest.badvalue'], [], w.data_cb, "badvalue")
        check(PMIX_SUCCESS == rc and w.event.wait(timeout=30) and
              PMIX_SUCCESS != w.status,
              "an answer that cannot be converted completes with an error")

    print("QUERY NB")
    w = nbwaiter("QUERY NB")
    rc = foo.query_nb([{'keys':[PMIX_QUERY_NAMESPACES], 'qualifiers':None}],
                      w.data_cb, "query")
    w.wait(foo, rc)

    print("LOG NB")
    w = nbwaiter("LOG NB")
    logdata = [{'key':PMIX_LOG_STDERR, 'value':'python log test\n',
                'val_type':PMIX_STRING}]
    rc = foo.log_nb(logdata, [], w.op_cb, "log")
    w.wait(foo, rc)

    # the remaining operations are ones our simple test server may well
    # decline. What is under test is that the arguments convert, the
    # request is accepted or refused cleanly, and the callback fires
    # exactly when the library said it would.
    print("JOB CONTROL NB")
    w = nbwaiter("JOB CONTROL NB")
    jdirs = [{'key':PMIX_JOB_CTRL_ID, 'value':'pyjob', 'val_type':PMIX_STRING}]
    rc = foo.job_control_nb(None, jdirs, w.data_cb, "jobctrl")
    w.wait(foo, rc)

    print("GET CREDENTIAL NB")
    w = nbwaiter("GET CREDENTIAL NB")
    rc = foo.get_credential_nb([], w.cred_cb, "getcred")
    if w.wait(foo, rc) and w.results is not None and 0 < len(w.results):
        print("VALIDATE CREDENTIAL NB")
        cred = w.results
        w = nbwaiter("VALIDATE CREDENTIAL NB")
        rc = foo.validate_credential_nb(cred, [], w.data_cb, "validate")
        w.wait(foo, rc)

    print("ALLOCATION REQUEST NB")
    w = nbwaiter("ALLOCATION REQUEST NB")
    rc = foo.allocation_request_nb(PMIX_ALLOC_NEW, [], w.data_cb, "alloc")
    w.wait(foo, rc)

    print("RESOURCE BLOCK NB")
    w = nbwaiter("RESOURCE BLOCK NB")
    units = [{'type': PMIX_DEVTYPE_GPU, 'count': 2}]
    rc = foo.resource_block_nb(PMIX_RESOURCE_BLOCK_DEFINE, "myblock",
                               units, [], w.op_cb, "resblock")
    done = w.wait(foo, rc)
    if 'scheduler' == host:
        check(done, "the scheduler completes a non-blocking resource block")

    print("SPAWN NB")
    w = nbwaiter("SPAWN NB")
    apps = [{'cmd': '/bin/true', 'argv': ['true'], 'maxprocs': 1,
             'env': ['PYTEST=1'], 'info': []}]
    rc = foo.spawn_nb([], apps, w.data_cb, "spawn")
    w.wait(foo, rc)

    print("CONNECT NB")
    w = nbwaiter("CONNECT NB")
    rc = foo.connect_nb(None, [], w.op_cb, "connect")
    if w.wait(foo, rc):
        print("DISCONNECT NB")
        w = nbwaiter("DISCONNECT NB")
        rc = foo.disconnect_nb(None, [], w.op_cb, "disconnect")
        w.wait(foo, rc)

    print("MONITOR NB")
    w = nbwaiter("MONITOR NB")
    # ask the server to watch us for heartbeats - the request itself is
    # the (valueless) monitor attribute, the timing rides in the directives
    monitor = [{'key': PMIX_MONITOR_HEARTBEAT, 'value': True,
                'val_type': PMIX_BOOL}]
    mdirs = [{'key': PMIX_MONITOR_ID, 'value': 'MONITOR1',
              'val_type': PMIX_STRING},
             {'key': PMIX_MONITOR_HEARTBEAT_TIME, 'value': 5,
              'val_type': PMIX_UINT32},
             {'key': PMIX_MONITOR_HEARTBEAT_DROPS, 'value': 2,
              'val_type': PMIX_UINT32}]
    rc = foo.monitor_nb(monitor, PMIX_MONITOR_HEARTBEAT_ALERT, mdirs,
                        w.data_cb, "monitor")
    if w.wait(foo, rc):
        # feed the monitor we just established
        print("Heartbeat result ", foo.error_string(foo.heartbeat()))
    # construct a group asynchronously. Passing None for the peers means
    # "everyone in my job"
    print("GROUP CONSTRUCT NB")
    dirs = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID,
             'value': True, 'val_type': PMIX_BOOL}]
    rc = foo.group_construct_nb("mygroup", None, dirs,
                                group_construct_cb, grpConstructed)
    print("Group construct_nb result ", foo.error_string(rc))
    if PMIX_SUCCESS == rc:
        if not grpConstructed.wait(timeout=30):
            print("GROUP CONSTRUCT TIMED OUT")
        elif PMIX_SUCCESS != grpStatus:
            # the host may not support group operations at all - the
            # scheduler test server, for one, does not
            print("Group construct failed: ", foo.error_string(grpStatus))
        else:
            print("Group construct results: ", grpResults)
            # and tear it down again, also asynchronously
            print("GROUP DESTRUCT NB")
            rc = foo.group_destruct_nb("mygroup", None,
                                       group_destruct_cb, grpDestructed)
            print("Group destruct_nb result ", foo.error_string(rc))
            if PMIX_SUCCESS == rc and not grpDestructed.wait(timeout=30):
                print("GROUP DESTRUCT TIMED OUT")
    # Wait for the model event, with a bound - as the group waits above have.
    #
    # This is the event the whole test turns on, and it is the one the test
    # is least sure of getting: the model is declared inside PMIx_Init, from
    # the PMIX_PROGRAMMING_MODEL/PMIX_MODEL_LIBRARY_NAME passed to it, while
    # the handler that catches it is not registered until several lines
    # after init returns.  Losing that race has to fail this test, not hang
    # it.  Unbounded, it wedged this process AND the scheduler that forked
    # us - which cannot finish reading our pipes until we close them - so CI
    # killed the pair after five minutes with no clue as to why.
    timedout = not termEvent.wait(timeout=30)
    if timedout:
        print("MODEL EVENT TIMED OUT")
    # deregistering drops the binding's record of the handler too
    rc = foo.deregister_event_handler(mymodelhndlr)
    print("DEREGISTER MODEL", foo.error_string(rc))
    dropped = PMIX_SUCCESS == rc and \
        all(mymodelhndlr != h['refid'] for h in pmix.myhdlrs)
    if not dropped:
        print("MODEL HANDLER STILL RECORDED AFTER DEREGISTRATION")
    # finalize
    info = []
    foo.finalize(info)
    print("Client finalize complete")
    if failures:
        print("FAILED CHECKS:", failures)
    if timedout or not replayed or not dropped or failures:
        exit(1)
if __name__ == '__main__':
    main()
