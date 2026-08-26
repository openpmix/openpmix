#!/usr/bin/env python3

from pmix import *
import signal, time
import os
import select
import subprocess

global killer

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

# Server-module upcall handlers. The binding invokes each registered
# handler with three arguments: the converted request, a Python wrapper
# around the C completion callback, and an opaque cbdata dict. A handler
# drives completion by invoking that callback and returns PMIX_SUCCESS.
def clientconnected(proc, cbfunc, cbdata):
    print("CLIENT CONNECTED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

def clientfinalized(proc, cbfunc, cbdata):
    print("CLIENT FINALIZED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)
    return PMIX_SUCCESS

def clientfence(args, cbfunc, cbdata):
    print("SERVER FENCE", args)
    # no data to return from this (empty) fence
    cbfunc(PMIX_SUCCESS, bytearray(0), cbdata)
    return PMIX_SUCCESS

def main():
    try:
        foo = PMIxServer()
    except:
        print("FAILED TO CREATE SERVER")
        exit(1)
    print("Testing server version ", foo.get_version())
    args = [{'key':PMIX_SERVER_SCHEDULER, 'value':'T', 'val_type':PMIX_BOOL}]
    map = {'clientconnected': clientconnected,
           'clientfinalized': clientfinalized,
           'fencenb': clientfence}
    my_result = foo.init(args, map)
    print("Testing PMIx_Initialized")
    rc = foo.initialized()
    print("Initialized: ", rc)
    vers = foo.get_version()
    print("Version: ", vers)

    # Register a fabric
    rc,fab = foo.fabric_register(None)
    print("Fabric registered: ", foo.error_string(rc))

    # setup the application
    (rc, regex) = foo.generate_regex(["test000","test001","test002"])
    print("Node regex, rc: ", regex, foo.error_string(rc))
    (rc, ppn) = foo.generate_ppn(["0,1,2", "3,4,5", "6,7"])
    print("PPN, rc: ", ppn, foo.error_string(rc))
    darray = {'type':PMIX_INFO, 'array':[{'key':PMIX_ALLOC_FABRIC_ID,
                            'value':'SIMPSCHED.net', 'val_type':PMIX_STRING},
                           {'key':PMIX_ALLOC_FABRIC_SEC_KEY, 'value':'T',
                            'val_type':PMIX_BOOL},
                           {'key':PMIX_SETUP_APP_ENVARS, 'value':'T',
                            'val_type':PMIX_BOOL}]}
    kyvals = [{'key':PMIX_NODE_MAP, 'value':regex, 'val_type':PMIX_STRING},
              {'key':PMIX_PROC_MAP, 'value':ppn, 'val_type':PMIX_STRING},
              {'key':PMIX_ALLOC_FABRIC, 'value':darray, 'val_type':PMIX_DATA_ARRAY}]

    appinfo = []
    rc, appinfo = foo.setup_application("SIMPSCHED", kyvals)
    print("SETUPAPP: ", appinfo)

    rc = foo.setup_local_support("SIMPSCHED", appinfo)
    print("SETUPLOCAL: ", foo.error_string(rc))

    # get our environment as a base
    env = os.environ.copy()

    # register an nspace for the client app
    kvals = [{'key':PMIX_NODE_MAP, 'value':regex, 'val_type':PMIX_STRING},
             {'key':PMIX_PROC_MAP, 'value':ppn, 'val_type':PMIX_STRING},
             {'key':PMIX_UNIV_SIZE, 'value':1, 'val_type':PMIX_UINT32},
             {'key':PMIX_JOB_SIZE, 'value':1, 'val_type':PMIX_UINT32}]
    print("REGISTERING NSPACE")
    rc = foo.register_nspace("testnspace", 1, kvals)
    print("RegNspace ", foo.error_string(rc))

    # register a client
    uid = os.getuid()
    gid = os.getgid()
    print("REGISTERING CLIENT")
    rc = foo.register_client({'nspace':"testnspace", 'rank':0}, uid, gid)
    print("RegClient ", foo.error_string(rc))

    # setup the fork
    rc = foo.setup_fork({'nspace':"testnspace", 'rank':0}, env)
    print("SetupFrk", foo.error_string(rc))

    # setup the client argv.  Locate the client beside this script
    # rather than in cwd: under a VPATH build we are run from the build
    # directory, which holds no .py files.
    args = [os.path.join(os.path.dirname(os.path.abspath(__file__)), "client.py")]
    # open a subprocess with stdout and stderr
    # as distinct pipes so we can capture their
    # output as the process runs
    p = subprocess.Popen(args, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Drain the child's pipes until both reach EOF, then reap it.
    #
    # Three things this has to get right, and the loop that was here got
    # none of them.  It must not wait forever on a child that has wedged:
    # select() with no timeout meant a stuck client wedged the scheduler
    # too, and CI killed the pair after five minutes with nothing to show
    # for it.  It must not stop reading while a pipe still holds data: the
    # done flags were reset on every iteration and cleared only for the
    # descriptors select() happened to return, so an EOF on stderr while
    # stdout was merely idle ended the loop and discarded the rest of the
    # client's output.  And it must look at how the child exited - nothing
    # ever reaped it, so a client that FAILED was reported as a pass.
    #
    # A pipe is dropped from the watch list when it reads EOF, so the loop
    # ends when the child has closed both and not before.  The idle timer
    # covers a child that stops talking; it is reset by any output, so a
    # slow client is not mistaken for a stuck one.
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
