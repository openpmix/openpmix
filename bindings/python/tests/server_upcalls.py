from pmix import *
import signal, time
global killer

# where local data is published for testing
pmix_locdata = []

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

def clientconnected(proc:tuple is not None):
    print("CLIENT CONNECTED", proc)
    return PMIX_OPERATION_SUCCEEDED

def clientfinalized(proc:tuple is not None):
    print("CLIENT FINALIZED", proc)
    return PMIX_OPERATION_SUCCEEDED

def clientfence(procs:list, directives:list, data:bytearray):
    # check directives
    print("CLIENTFENCE")
    output = bytearray(0)
    if directives is not None:
        for d in directives:
            # these are each an info dict
            if "pmix" not in d['key']:
                # we do not support such directives - see if
                # it is required
                try:
                    if d['flags'] & PMIX_INFO_REQD:
                        # return an error
                        return PMIX_ERR_NOT_SUPPORTED, output
                except:
                    #it can be ignored
                    pass
    print("COMPLETE")
    return PMIX_OPERATION_SUCCEEDED, output

def clientpublish(proc:dict, directives:list):
    print("SERVER: PUBLISH")
    for d in directives:
        pdata = {}
        pdata['proc'] = proc
        pdata['key']            = d['key']
        pdata['value']          = d['value']
        pdata['val_type']       = d['val_type']
        pmix_locdata.append(pdata)
    return PMIX_OPERATION_SUCCEEDED

def clientunpublish(proc:dict, pykeys:list, directives:list):
    print("SERVER: UNPUBLISH")
    for k in pykeys:
        for d in pmix_locdata:
            if k.decode('ascii') == d['key']:
                pmix_locdata.remove(d)
    return PMIX_OPERATION_SUCCEEDED

def clientlookup(proc:dict, keys:list, directives:list):
    print("SERVER: LOOKUP")
    ret_pdata = []
    for k in keys:
        for d in pmix_locdata:
            if k.decode('ascii') == d['key']:
                ret_pdata.append(d)
    # return rc and pdata
    return PMIX_SUCCESS, ret_pdata

def clientquery(proc:dict, queries:list):
    print("SERVER: QUERY")
    # return a python info list of dictionaries
    info = {}
    results = []
    # find key we passed in to client, and
    # if it matches return fake PSET_NAME
    # since RM actually assigns this, we
    # just return arbitrary name if key is
    # found
    find_str = 'pmix.qry.psets'
    for q in queries:
        for k in q['keys']:
            if k == find_str:
                info = {'key': find_str, 'value': 'PSET_NAME', 'val_type': PMIX_STRING}
                results.append(info)
    return PMIX_ERR_NOT_FOUND, results

def client_register_events(codes:list, directives:list):
    print("CLIENT REGISTER EVENTS ", codes)
    return PMIX_OPERATION_SUCCEEDED
