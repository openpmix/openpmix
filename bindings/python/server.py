#!/usr/bin/env python3

from pmix import *
import signal, time
import os
import select
import subprocess

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
    return ret_pdata, PMIX_SUCCESS

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
           'unpublish': clientunpublish,
           'lookup': clientlookup}
    my_result = foo.init(args, map)
    print("Testing PMIx_Initialized")
    rc = foo.initialized()
    print("Initialized: ", rc)
    vers = foo.get_version()
    print("Version: ", vers)

    # pdata = {‘proc’: {‘nspace’: mynspace, ‘rank’: myrank}, ‘key’: ky,
    # ‘value’: v, ‘val_type’: t}
    pmix_locdat = []

    # get our environment as a base
    env = os.environ.copy()
    # register an nspace for the client app
    darray = {'type':PMIX_SIZE, 'array':[1, 2, 3, 4, 5]}
    kvals = [{'key':'testkey', 'value':darray, 'val_type':PMIX_DATA_ARRAY}]
    print("REGISTERING NSPACE")
    rc = foo.register_nspace("testnspace", 1, kvals)
    print("RegNspace ", rc)

    # register a client
    uid = os.getuid()
    gid = os.getgid()
    rc = foo.register_client({'nspace':"testnspace", 'rank':0}, uid, gid)
    print("RegClient ", rc)
    # setup the fork
    rc = foo.setup_fork({'nspace':"testnspace", 'rank':0}, env)
    print("SetupFrk", rc)

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
