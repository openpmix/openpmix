#!/usr/bin/env python3

from pmix import *
import signal, time
import asyncio
from concurrent.futures import ThreadPoolExecutor
global killer

# Our event loop
event_loop = None

# where local data is published for testing
pmix_locdata = []

def start_event_loop():
    global event_loop
    if None == event_loop:
        event_loop = asyncio.new_event_loop()
        ThreadPoolExecutor().submit(event_loop.run_forever)

def stop_event_loop():
    global event_loop
    event_loop.call_soon_threadsafe(event_loop.stop)
    while event_loop.is_running():
        time.sleep(0.01)
    event_loop.close()

class GracefulKiller:
  kill_now = False
  def __init__(self):
    signal.signal(signal.SIGINT, self.exit_gracefully)
    signal.signal(signal.SIGTERM, self.exit_gracefully)

  def exit_gracefully(self,signum, frame):
    self.kill_now = True

def _clientconnected(proc:tuple is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT CONNECTED SHIFTED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)

def clientconnected(proc:tuple is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT CONNECTED", proc)
    event_loop.call_soon_threadsafe(_clientconnected, proc, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientfinalized(proc:tuple is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT FINALIZED SHIFTED", proc)
    cbfunc(PMIX_SUCCESS, cbdata)

def clientfinalized(proc:tuple is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT FINALIZED", proc)
    event_loop.call_soon_threadsafe(_clientfinalized, proc, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientfence(args:dict is not None, cbfunc:pypmix_modex_cbfunc_t, cbdata:dict):
    # check directives
    print("CLIENTFENCE SHIFTED")
    output = bytearray(0)
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
    print("COMPLETE")
    cbfunc(PMIX_SUCCESS, output, cbdata)

def clientfence(args:dict is not None, cbfunc:pypmix_modex_cbfunc_t, cbdata:dict):
    # check directives
    print("CLIENTFENCE")
    event_loop.call_soon_threadsafe(_clientfence, args, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientpublish(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("SERVER: PUBLISH SHIFTED")
    for d in args['directives']:
        pdata = {}
        pdata['proc'] = args['proc']
        pdata['key']            = d['key']
        pdata['value']          = d['value']
        pdata['val_type']       = d['val_type']
        pmix_locdata.append(pdata)
    cbfunc(PMIX_SUCCESS, cbdata)


def clientpublish(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("SERVER: PUBLISH")
    event_loop.call_soon_threadsafe(_clientpublish, args, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientunpublish(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("SERVER: UNPUBLISH SHIFTED")
    for k in args['keys']:
        for d in pmix_locdata:
            if k == d['key']:
                pmix_locdata.remove(d)
    cbfunc(PMIX_SUCCESS, cbdata)

def clientunpublish(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("SERVER: UNPUBLISH")
    event_loop.call_soon_threadsafe(_clientunpublish, args, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientlookup(args:dict is not None, cbfunc:pypmix_lookup_cbfunc_t, cbdata:dict):
    print("SERVER: LOOKUP SHIFTED")
    ret_pdata = []
    for k in args['keys']:
        for d in pmix_locdata:
            if k.decode('ascii') == d['key']:
                ret_pdata.append(d)
    # return rc and pdata
    cbfunc(PMIX_SUCCESS, ret_pdata, cbdata)

def clientlookup(args:dict is not None, cbfunc:pypmix_lookup_cbfunc_t, cbdata:dict):
    print("SERVER: LOOKUP")
    event_loop.call_soon_threadsafe(_clientlookup, args, cbfunc, cbdata)
    return PMIX_SUCCESS

def _clientquery(args:dict is not None, cbfunc:pypmix_info_cbfunc_t, cbdata:dict):
    print("SERVER: QUERY SHIFTED")
    # return a python info list of dictionaries
    info = {}
    results = []
    rc = PMIX_ERR_NOT_FOUND
    # find key we passed in to client, and
    # if it matches return fake PSET_NAME
    # since RM actually assigns this, we
    # just return arbitrary name if key is
    # found
    find_str = 'pmix.qry.psets'
    for q in args['queries']:
        for k in q['keys']:
            if k == find_str:
                info = {'key': find_str, 'value': 'PSET_NAME', 'val_type': PMIX_STRING}
                results.append(info)
                rc = PMIX_SUCCESS
    cbfunc(rc, results, cbdata)

def clientquery(args:dict is not None, cbfunc:pypmix_info_cbfunc_t, cbdata:dict):
    print("SERVER: QUERY")
    event_loop.call_soon_threadsafe(_clientquery, args, cbfunc, cbdata)
    return PMIX_SUCCESS

def _client_register_events(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT REGISTER EVENTS SHIFTED", args['codes'])
    cbfunc(PMIX_SUCCESS, cbdata)


def client_register_events(args:dict is not None, cbfunc:pypmix_op_cbfunc_t, cbdata:dict):
    print("CLIENT REGISTER EVENTS", args['codes'])
    event_loop.call_soon_threadsafe(_client_register_events, args, cbfunc, cbdata)
    return PMIX_SUCCESS
