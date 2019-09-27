#!/usr/bin/env python3

from pmix import *

def main():
    foo = PMIxClient()
    print("Testing PMIx ", foo.get_version())
    info = [{'key':PMIX_PROGRAMMING_MODEL, 'value':'TEST', 'val_type':PMIX_STRING},
            {'key':PMIX_MODEL_LIBRARY_NAME, 'value':'PMIX', 'val_type':PMIX_STRING}]
    my_result,myname = foo.init(info)
    print("Init result ", my_result)
    if 0 != my_result:
        print("FAILED TO INIT")
        exit(1)

    # try putting something
    print("PUT")
    rc = foo.put(PMIX_GLOBAL, "mykey", {'value':1, 'val_type':PMIX_INT32})
    print("Put result ", rc);
    # commit it
    print("COMMIT")
    rc = foo.commit()
    print ("Commit result ", rc)
    # execute fence
    print("FENCE")
    procs = []
    info = []
    rc = foo.fence(procs, info)
    print("GET")
    info = []
    rc, get_val = foo.get({'nspace':"testnspace", 'rank': 0}, "mykey", info)
    print("Get result: ", rc)
    print("Get value returned: ", get_val)
    # test a fence that should return not_supported because
    # we pass a required attribute that doesn't exist
    procs = []
    info = [{'key': 'ARBITRARY', 'flags': PMIX_INFO_REQD, 'value':10, 'val_type':PMIX_INT}]
    rc = foo.fence(procs, info)
    print("FENCE COMPLETE")
    print("Fence should be not supported", foo.error_string(rc))
    info = [{'key': 'ARBITRARY', 'flags': None, 'value':10, 'val_type':PMIX_INT}]
    rc = foo.publish(info)
    print("Publish result: ", foo.error_string(rc))
    rc = foo.fence(procs, info)
    pdata_key = [{'key':'ARBITRARY'}]
    rc,pdata = foo.lookup(pdata_key, None)
    rc = foo.lookup(pdata_key, None)
    print("lookup result: ", rc, pdata)
    pykeys = ['ARBITRARY']
    info = []
    rc = foo.unpublish(pykeys, info)
    print("unpublish result: ", foo.error_string(rc))
    rc = foo.fence(procs, info)
    print("QUERY INFO")
    pyq = [{'keys':[PMIX_QUERY_PSET_NAMES], 'qualifiers':[]}]
    rc, pyresults = foo.query(pyq)
    print("query info result: ", foo.error_string(rc))
    print("pyresults: ", pyresults)
    # finalize
    info = []
    foo.finalize(info)
    print("Client finalize complete")
if __name__ == '__main__':
    main()
