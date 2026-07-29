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
    # test a fence that should return not_supported because
    # we pass a required attribute that doesn't exist
    procs = []
    info = [{'key': 'ARBITRARY', 'flags': PMIX_INFO_REQD, 'value':10, 'val_type':PMIX_INT}]
    rc = foo.fence(procs, info)
    print("Fence should be not supported:", foo.error_string(rc))
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
