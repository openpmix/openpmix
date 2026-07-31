#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Support code shared by the Python examples - the counterpart of the C
# examples' "examples.h".
#
# The C header exists because a C program driving an asynchronous library
# has to hand-roll a condition variable for every "call this, then wait for
# the callback" sequence.  Python already has threading.Event, so the
# classes here are thin wrappers that add the one thing Event lacks: a
# place to stash the status and payload the callback was handed.
#
# Two differences from the C examples are worth knowing before reading any
# of the ports:
#
#  1. Event-handler registration is synchronous in Python.  The C examples
#     all carry an "evhandler_reg_callbk" plus a lock to wait on it;
#     PMIxClient.register_event_handler() already blocks and returns
#     (rc, refid), so the ports simply drop that machinery.
#
#  2. PMIX_EVENT_RETURN_OBJECT cannot cross the binding.  The C examples
#     pass a pointer to a stack object as PMIX_POINTER and fish it back out
#     of the handler's info array.  Python has no such pointer, so the ports
#     use an ordinary closure or module-global instead - which is what a
#     Python programmer would reach for anyway.

import os
import sys
import glob
import threading


def locate_pmix():
    """Put the in-tree built bindings on sys.path if pmix isn't importable.

    Lets an example run straight out of a built-but-not-installed tree with
    no PYTHONPATH set, exactly as test/python/test_bindings.py does.
    """
    try:
        import pmix  # noqa: F401
        return True
    except ImportError:
        pass
    here = os.path.dirname(os.path.abspath(__file__))
    # examples/python -> repo root -> bindings/python/build/lib.*
    root = os.path.abspath(os.path.join(here, "..", ".."))
    patterns = [
        os.path.join(root, "bindings", "python", "build", "lib.*"),
        os.path.join(root, "bindings", "python"),
    ]
    for pat in patterns:
        for d in sorted(glob.glob(pat)):
            if glob.glob(os.path.join(d, "pmix*.so")):
                sys.path.insert(0, d)
                return True
    return False


if not locate_pmix():
    print("The PMIx Python bindings could not be found.\n"
          "Configure the tree with --enable-python-bindings and rebuild, or\n"
          "set PYTHONPATH to the directory holding the pmix extension module.",
          file=sys.stderr)
    sys.exit(1)

from pmix import *  # noqa: E402,F401,F403


class MyLock:
    """The Python stand-in for the C examples' mylock_t.

    A callback records what it was given and calls wakeup(); the main
    thread calls wait().  The extra fields mirror the C struct so the ports
    read the same way: 'status' for the operation's result, 'count' for the
    handful of examples that tally callbacks, 'answer' for a returned
    string, and 'evhandler_ref' for a registration index.
    """

    def __init__(self):
        self.event = threading.Event()
        self.status = PMIX_SUCCESS
        self.count = 0
        self.answer = None
        self.evhandler_ref = 0

    def wait(self, timeout=None):
        """Block until wakeup() is called.  Returns False on timeout."""
        return self.event.wait(timeout=timeout)

    def wakeup(self, status=None):
        """Release whoever is blocked in wait()."""
        if status is not None:
            self.status = status
        self.event.set()

    def clear(self):
        """Rearm the lock for another operation."""
        self.event.clear()


class MyQuery(MyLock):
    """The Python stand-in for the C examples' myquery_data_t.

    Adds the info list a query callback hands back.  The C version has to
    deep-copy the returned pmix_info_t array before the library reclaims
    it; the binding has already converted it to a list of dicts that Python
    owns, so the port just keeps the reference.
    """

    def __init__(self):
        super().__init__()
        self.status = PMIX_ERROR
        self.info = []


class MyRel(MyLock):
    """The Python stand-in for the C examples' myrel_t.

    Carries the namespace whose termination we are waiting on, plus the
    exit code the event reported.
    """

    def __init__(self, nspace=None):
        super().__init__()
        self.nspace = nspace
        self.exit_code = 0
        self.exit_code_given = False


def eprint(*args, **kwargs):
    """The C examples write their progress to stderr; so do these."""
    kwargs.setdefault("file", sys.stderr)
    print(*args, **kwargs)
    sys.stderr.flush()


def as_key(key):
    """Normalize a key to str for comparison.

    Worth knowing: the generated PMIX_* attribute constants are **bytes**
    (b'pmix.alloc.status'), because that is the form the conversion layer
    hands to C. Keys coming *back* from the library have been decoded to
    str. So the obvious

        if PMIX_ALLOC_STATUS == info[n]['key']:

    is never true - it compares bytes to str and silently finds nothing.
    Everything below goes through here so the comparison works whichever
    side supplies which.
    """
    if isinstance(key, bytes):
        return key.decode('ascii')
    return key


def key_is(key, name):
    """True if an info dict's key is the PMIx attribute 'name'."""
    return as_key(key) == as_key(name)


def find_key(info, key):
    """Return the value carried by 'key' in an info list, or None.

    The C examples spell this as a loop over PMIX_CHECK_KEY; the info list
    a Python handler receives is a list of {'key','value','val_type'}
    dicts, so it is one loop.
    """
    if info is None:
        return None
    for item in info:
        if key_is(item['key'], key):
            return item['value']
    return None
