#!/usr/bin/env python3

from pmix import *
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

def main():
    global termEvent
    
    foo = PMIxClient()
    print("Testing PMIx ", foo.get_version())
    info = [{'key':PMIX_PROGRAMMING_MODEL, 'value':'TEST', 'val_type':PMIX_STRING},
            {'key':PMIX_MODEL_LIBRARY_NAME, 'value':'PMIX', 'val_type':PMIX_STRING}]
    my_result = foo.init(info)
    print("Init result ", my_result)
    if 0 != my_result[0]:
        print("FAILED TO INIT")
        exit(1)
    # register an event handler
    rc,myevhndlr = foo.register_event_handler(None, None, default_evhandler)
    print("REGISTER DEFAULT", foo.error_string(rc))
    # register the model handler
    rc,mymodelhndlr = foo.register_event_handler([PMIX_MODEL_DECLARED], None, model_evhandler)
    print("REGISTER MODEL", foo.error_string(rc))

    # try putting something
    print("PUT")
    rc = foo.put(PMIX_GLOBAL, "mykey", {'value':1, 'val_type':PMIX_INT32})
    print("Put result ",foo.error_string(rc));
    # commit it
    print("COMMIT")
    rc = foo.commit()
    print ("Commit result ", foo.error_string(rc))
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
    w.wait(foo, rc)

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
    # wait for model event
    while not termEvent.is_set():
        time.sleep(0.001)
    # finalize
    info = []
    foo.finalize(info)
    print("Client finalize complete")
if __name__ == '__main__':
    main()
