#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/log.c
#
# Rank 0 emits one log message through PMIx_Log. Which channel it uses is
# chosen on the command line:
#
#   (no argument)         log to stderr
#   --syslog              log to the local syslog
#   --global-syslog       log to the global syslog
#   --email <address>     send the message as email
#
# The email form shows a directive that carries a whole nested set of
# directives - a PMIX_DATA_ARRAY of infos.

import socket
import sys

from examples import *


def main():
    host = socket.gethostname()

    syslog = False
    global_syslog = False
    email = None

    # check for CLI directives
    n = 1
    while n < len(sys.argv):
        if "--syslog" == sys.argv[n]:
            syslog = True
            break
        elif "--global-syslog" == sys.argv[n]:
            global_syslog = True
            break
        elif "--email" == sys.argv[n]:
            if n + 1 >= len(sys.argv):
                eprint("Must provide email target")
                sys.exit(1)
            email = sys.argv[n + 1]
            break
        n += 1

    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM.
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc and PMIX_ERR_UNREACH != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(0)
    if PMIX_ERR_UNREACH == rc:
        myproc = {'nspace': "UNKNOWN", 'rank': PMIX_RANK_UNDEF}
        eprint("Client ns %s rank %d: Running as singleton on host %s"
               % (myproc['nspace'], myproc['rank'], host))
    else:
        eprint("Client ns %s rank %d: Running on host %s"
               % (myproc['nspace'], myproc['rank'], host))

    # have rank 0 do the logs - doesn't really matter who does it
    if 0 == myproc['rank']:
        timestamp = [{'key': PMIX_LOG_GENERATE_TIMESTAMP, 'value': True,
                      'val_type': PMIX_BOOL}]
        if syslog:
            # if requested, output one to syslog
            eprint("LOG TO LOCAL SYSLOG")
            info = [{'key': PMIX_LOG_LOCAL_SYSLOG, 'value': "SYSLOG message\n",
                     'val_type': PMIX_STRING}]
            rc = client.log(info, timestamp)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Log syslog failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
        elif global_syslog:
            eprint("LOG TO GLOBAL SYSLOG")
            info = [{'key': PMIX_LOG_GLOBAL_SYSLOG,
                     'value': "GLOBAL SYSLOG message\n",
                     'val_type': PMIX_STRING}]
            rc = client.log(info, None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Log GLOBAL syslog "
                       "failed: %s" % (myproc['nspace'], myproc['rank'],
                                       client.error_string(rc)))
        elif email is not None:
            eprint("LOG TO EMAIL %s" % email)
            # the email directives ride inside the log request as a
            # data array of infos
            maildirs = [
                {'key': PMIX_LOG_EMAIL_ADDR, 'value': email,
                 'val_type': PMIX_STRING},
                {'key': PMIX_LOG_EMAIL_SUBJECT, 'value': "TEST EMAIL",
                 'val_type': PMIX_STRING},
                {'key': PMIX_LOG_MSG, 'value': "SEE IF THIS WORKS",
                 'val_type': PMIX_STRING},
                {'key': PMIX_LOG_EMAIL_SENDER_ADDR, 'value': email,
                 'val_type': PMIX_STRING}]
            info = [{'key': PMIX_LOG_EMAIL,
                     'value': {'type': PMIX_INFO, 'array': maildirs},
                     'val_type': PMIX_DATA_ARRAY}]
            rc = client.log(info, timestamp)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Log EMAIL failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
        else:
            # output a log message to stderr
            eprint("LOG TO STDERR")
            info = [{'key': PMIX_LOG_STDERR, 'value': "stderr log message\n",
                     'val_type': PMIX_STRING}]
            rc = client.log(info, timestamp)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Log stderr failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))

    eprint("%s:%d Calling Fence" % (myproc['nspace'], myproc['rank']))
    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))

    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
