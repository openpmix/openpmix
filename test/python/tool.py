#!/usr/bin/env python3

from pmix import *
import os
import time

def main():
    foo = PMIxTool()
    print("Testing PMIx Tool ", foo.get_version())
    # construct a session tmpdir
    uid = os.geteuid()
    gid = os.getegid()
    basename = os.path.basename(__file__)
    # a bare os.getenv('TMPDIR') is None on a machine that does not set it,
    # and os.path.join(None, ...) raises rather than reporting anything
    # useful
    tmpdir = os.getenv('TMPDIR', '/tmp')
    sessiondir = os.path.join(tmpdir, basename + '.session.' + str(uid) + '.' + str(gid))
    info = [{'key':PMIX_LAUNCHER, 'value':True, 'val_type':PMIX_BOOL},
            {'key':PMIX_SERVER_TOOL_SUPPORT, 'value':True, 'val_type':PMIX_BOOL},
            {'key':PMIX_SERVER_TMPDIR, 'value':sessiondir, 'val_type':PMIX_STRING}]
    (my_result, myproc) = foo.init(info)
    if 0 != my_result:
        print("FAILED TO INIT", foo.error_string(my_result))
        exit(1)

    # construct an application to spawn
    app = {'cmd':'hostname', 'maxprocs':1}
    rc, nspace = foo.spawn(None, [app])
    if PMIX_SUCCESS != rc:
        print("SPAWN FAILED", foo.error_string(rc))
    else:
        print("SPAWNED", nspace)
    time.sleep(2.0)
    # finalize
    foo.finalize()
    print("Tool finalize complete")
if __name__ == '__main__':
    main()
