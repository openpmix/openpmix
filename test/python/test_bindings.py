#!/usr/bin/env python3
#
# Copyright (c) 2025      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Standalone unit tests for the PMIx Python bindings.
#
# These tests exercise only the parts of the binding that do NOT require a
# live PMIx server: module import, the class hierarchy, constant definitions,
# and the many stateless string/pretty-print converters.  The existing
# client.py/server.py/sched.py scripts cover the connected round-trip paths;
# this file provides fast, self-contained coverage that runs under `make
# check` (when the extension is already on PYTHONPATH) or directly from a
# built-but-not-installed tree.
#
# Run directly with:
#     PYTHONPATH=<dir containing pmix*.so> ./test_bindings.py
# or, from an in-tree build, simply:
#     ./test_bindings.py
# (the module search below will locate the freshly built extension).

import os
import sys
import glob
import unittest


def _locate_extension():
    """Add the in-tree built extension to sys.path if pmix isn't importable.

    Mirrors what the autotools-generated run scripts do via
    PMIX_PYTHON_EGG_PATH, but also works when a developer runs this file by
    hand from a build tree that has not been installed.
    """
    try:
        import pmix  # noqa: F401
        return
    except ImportError:
        pass
    here = os.path.dirname(os.path.abspath(__file__))
    # test/python -> repo root -> bindings/python/build/lib.*
    root = os.path.abspath(os.path.join(here, "..", ".."))
    patterns = [
        os.path.join(root, "bindings", "python", "build", "lib.*"),
        os.path.join(root, "bindings", "python"),
    ]
    for pat in patterns:
        for d in sorted(glob.glob(pat)):
            if glob.glob(os.path.join(d, "pmix*.so")):
                sys.path.insert(0, d)
                return


_locate_extension()

try:
    import pmix
except ImportError as e:  # pragma: no cover - environment problem
    sys.stderr.write("Unable to import the pmix extension: %s\n" % e)
    sys.stderr.write("Set PYTHONPATH to the directory holding pmix*.so.\n")
    # Exit 77 is the automake "SKIP" code
    sys.exit(77)


class TestModule(unittest.TestCase):
    def test_classes_present(self):
        for name in ("PMIxClient", "PMIxServer", "PMIxTool", "PMIxScheduler"):
            self.assertTrue(hasattr(pmix, name), "missing class " + name)

    def test_class_hierarchy(self):
        # The role classes form an inheritance chain
        self.assertTrue(issubclass(pmix.PMIxServer, pmix.PMIxClient))
        self.assertTrue(issubclass(pmix.PMIxTool, pmix.PMIxServer))
        self.assertTrue(issubclass(pmix.PMIxScheduler, pmix.PMIxTool))

    def test_construct(self):
        # Every role must be constructible without a server
        for cls in (pmix.PMIxClient, pmix.PMIxServer,
                    pmix.PMIxTool, pmix.PMIxScheduler):
            obj = cls()
            self.assertIsNotNone(obj)

    def test_star_import_does_not_rebind_the_callers_modules(self):
        # "from pmix import *" is the documented way to use these bindings,
        # so what it exports is part of the interface.  Without an __all__
        # it handed the caller every name at module scope, including the
        # modules pmix.pyx imports for its own use - a program that did
        # "import time" and then "from pmix import *" silently got the
        # extension's time, and the same for os, sys, array, queue, signal,
        # threading, ctypes and traceback.
        import types
        namespace = {}
        exec("from pmix import *", namespace)
        modules = sorted(name for name, obj in namespace.items()
                         if isinstance(obj, types.ModuleType)
                         and not name.startswith("_"))
        self.assertEqual(modules, [],
                         "star import rebinds the caller's %s" % modules)

    def test_star_import_still_exports_what_it_should(self):
        namespace = {}
        exec("from pmix import *", namespace)
        for name in ("PMIxClient", "PMIxServer", "PMIxTool", "PMIxScheduler",
                     "PMIX_SUCCESS", "PMIX_RANK_WILDCARD", "PMIX_STRING",
                     "PMIX_ERR_NOT_FOUND", "PMIX_JOB_SIZE"):
            self.assertIn(name, namespace, "star import dropped " + name)
        # the constants are the bulk of the module, so a mistake in the
        # export list would show up as a collapse in their number
        constants = [n for n in namespace if n.startswith("PMIX_")]
        self.assertGreater(len(constants), 500, len(constants))


class TestConstants(unittest.TestCase):
    def test_success_is_zero(self):
        self.assertEqual(pmix.PMIX_SUCCESS, 0)

    def test_error_codes_negative(self):
        for name in ("PMIX_ERR_NOT_SUPPORTED", "PMIX_ERR_NOT_FOUND",
                     "PMIX_ERR_BAD_PARAM", "PMIX_ERR_NOMEM"):
            self.assertLess(getattr(pmix, name), 0, name + " should be < 0")

    def test_rank_wildcard(self):
        # PMIX_RANK_WILDCARD is UINT32_MAX - 1
        self.assertEqual(pmix.PMIX_RANK_WILDCARD, 0xFFFFFFFF - 1)

    def test_rank_undef(self):
        self.assertEqual(pmix.PMIX_RANK_UNDEF, 0xFFFFFFFF)

    def test_max_lengths(self):
        # These bound the fixed-size nspace/key arrays in the C structs
        self.assertGreater(pmix.PMIX_MAX_NSLEN, 0)
        self.assertGreater(pmix.PMIX_MAX_KEYLEN, 0)

    def test_datatype_constants_distinct(self):
        # A representative spread of data-type constants must all differ
        types = [pmix.PMIX_INT, pmix.PMIX_INT32, pmix.PMIX_UINT64,
                 pmix.PMIX_STRING, pmix.PMIX_BOOL, pmix.PMIX_PROC,
                 pmix.PMIX_INFO, pmix.PMIX_DATA_ARRAY]
        self.assertEqual(len(types), len(set(types)))

    def test_scope_constants(self):
        self.assertNotEqual(pmix.PMIX_LOCAL, pmix.PMIX_REMOTE)
        self.assertNotEqual(pmix.PMIX_GLOBAL, pmix.PMIX_LOCAL)


class TestVersion(unittest.TestCase):
    def setUp(self):
        self.client = pmix.PMIxClient()

    def test_get_version_nonempty(self):
        v = self.client.get_version()
        self.assertTrue(len(v) > 0)

    def test_get_version_is_str(self):
        # get_version decodes to str
        self.assertIsInstance(self.client.get_version(), str)

    def test_get_version_mentions_pmix(self):
        v = self.client.get_version()
        if isinstance(v, bytes):
            v = v.decode("utf-8", "replace")
        self.assertIn("PMIx", v)

    def test_initialized_false_before_init(self):
        # Must be falsey and must not raise before PMIx_Init has run
        self.assertFalse(self.client.initialized())


class TestStringConverters(unittest.TestCase):
    """The *_string helpers are pure lookups in the C library and are safe to
    call with no server and no init()."""

    def setUp(self):
        self.c = pmix.PMIxClient()

    def _check(self, s):
        if isinstance(s, bytes):
            s = s.decode("utf-8", "replace")
        self.assertIsInstance(s, str)
        self.assertTrue(len(s) > 0)
        return s

    def test_error_string_known(self):
        self.assertEqual(self._check(self.c.error_string(pmix.PMIX_SUCCESS)),
                         "PMIX_SUCCESS")
        self.assertEqual(
            self._check(self.c.error_string(pmix.PMIX_ERR_NOT_SUPPORTED)),
            "PMIX_ERR_NOT_SUPPORTED")

    def test_data_type_string_known(self):
        self.assertEqual(
            self._check(self.c.data_type_string(pmix.PMIX_INT32)),
            "PMIX_INT32")

    def test_scope_string(self):
        self._check(self.c.scope_string(pmix.PMIX_GLOBAL))

    def test_persistence_string(self):
        self._check(self.c.persistence_string(pmix.PMIX_PERSIST_SESSION))

    def test_data_range_string(self):
        self._check(self.c.data_range_string(pmix.PMIX_RANGE_LOCAL))

    def test_proc_state_string(self):
        self._check(
            self.c.proc_state_string(pmix.PMIX_PROC_STATE_UNTERMINATED))

    def test_job_state_string(self):
        self._check(self.c.job_state_string(pmix.PMIX_JOB_STATE_RUNNING))

    def test_alloc_directive_string(self):
        self._check(self.c.alloc_directive_string(pmix.PMIX_ALLOC_NEW))

    def test_iof_channel_string(self):
        self._check(self.c.iof_channel_string(pmix.PMIX_FWD_STDOUT_CHANNEL))

    def test_info_directives_string(self):
        self._check(self.c.info_directives_string(pmix.PMIX_INFO_REQD))

    def test_attribute_name_roundtrip(self):
        # get_attribute_name maps a string key back to its PMIX_ symbol.
        name = self.c.get_attribute_name("pmix.rank")
        if isinstance(name, bytes):
            name = name.decode("utf-8", "replace")
        self.assertIsInstance(name, str)

    def test_value_comparison_string(self):
        self._check(self.c.value_comparison_string(pmix.PMIX_EQUAL))
        self._check(self.c.value_comparison_string(pmix.PMIX_VALUE1_GREATER))

    def test_group_operation_string(self):
        self._check(self.c.group_operation_string(pmix.PMIX_GROUP_CONSTRUCT))
        self._check(self.c.group_operation_string(pmix.PMIX_GROUP_DESTRUCT))

    def test_resource_block_directive_string(self):
        self._check(self.c.resource_block_directive_string(
            pmix.PMIX_RESOURCE_BLOCK_DEFINE))
        self._check(self.c.resource_block_directive_string(
            pmix.PMIX_RESOURCE_BLOCK_DELETE))

    def test_alloc_inheritance_string(self):
        self._check(self.c.alloc_inheritance_string(
            pmix.PMIX_ALLOC_INHERIT_NONE))
        self._check(self.c.alloc_inheritance_string(
            pmix.PMIX_ALLOC_INHERIT_CHILD))

    def test_error_code_is_inverse_of_error_string(self):
        # error_code is the one converter that runs the other way
        for code in (pmix.PMIX_SUCCESS, pmix.PMIX_ERR_NOT_SUPPORTED,
                     pmix.PMIX_ERR_TIMEOUT, pmix.PMIX_ERR_BAD_PARAM):
            name = self.c.error_string(code)
            if isinstance(name, bytes):
                name = name.decode("utf-8", "replace")
            self.assertEqual(self.c.error_code(name), code,
                             "error_code(%s) did not return %d" % (name, code))

    def test_error_code_rejects_bad_input(self):
        self.assertEqual(self.c.error_code(None), pmix.PMIX_ERR_BAD_PARAM)
        # an unknown name is simply not found - it must not raise
        self.assertIsInstance(self.c.error_code("NO_SUCH_STATUS_NAME"), int)


class TestCpusetConverters(unittest.TestCase):
    """The cpuset dict <-> string converters. These are pure Python helpers
    exposed at module scope precisely so the range expansion - the only part
    with real logic - can be covered without a server."""

    def test_from_string_range(self):
        self.assertEqual(pmix.pmix_cpuset_from_string("hwloc:0-3,8"),
                         {"source": "hwloc", "cpus": [0, 1, 2, 3, 8]})

    def test_from_string_single(self):
        self.assertEqual(pmix.pmix_cpuset_from_string("hwloc:5"),
                         {"source": "hwloc", "cpus": [5]})

    def test_from_string_empty_set(self):
        self.assertEqual(pmix.pmix_cpuset_from_string("hwloc:"),
                         {"source": "hwloc", "cpus": []})

    def test_from_string_accepts_bytes(self):
        self.assertEqual(pmix.pmix_cpuset_from_string(b"hwloc:1,2"),
                         {"source": "hwloc", "cpus": [1, 2]})

    def test_from_string_rejects_missing_delimiter(self):
        self.assertIsNone(pmix.pmix_cpuset_from_string("nodelimiter"))

    def test_from_string_rejects_reversed_range(self):
        self.assertIsNone(pmix.pmix_cpuset_from_string("hwloc:5-2"))

    def test_from_string_rejects_non_numeric(self):
        self.assertIsNone(pmix.pmix_cpuset_from_string("hwloc:a-b"))
        self.assertIsNone(pmix.pmix_cpuset_from_string("hwloc:zero"))

    def test_from_string_rejects_non_string(self):
        self.assertIsNone(pmix.pmix_cpuset_from_string(None))
        self.assertIsNone(pmix.pmix_cpuset_from_string(17))

    def test_to_string_from_indices(self):
        self.assertEqual(
            pmix.pmix_cpuset_to_string({"source": "hwloc", "cpus": [0, 1, 2, 8]}),
            "hwloc:0,1,2,8")

    def test_to_string_from_range_tokens(self):
        self.assertEqual(
            pmix.pmix_cpuset_to_string({"source": "hwloc", "cpus": ["0-3", "8"]}),
            "hwloc:0-3,8")

    def test_to_string_from_preformatted(self):
        self.assertEqual(
            pmix.pmix_cpuset_to_string({"source": "hwloc", "cpus": "0-3,8"}),
            "hwloc:0-3,8")

    def test_to_string_defaults_source(self):
        self.assertEqual(pmix.pmix_cpuset_to_string({"cpus": [1]}), "hwloc:1")

    def test_to_string_rejects_non_dict(self):
        self.assertIsNone(pmix.pmix_cpuset_to_string("notadict"))
        self.assertIsNone(pmix.pmix_cpuset_to_string(None))

    def test_to_string_rejects_empty_source(self):
        self.assertIsNone(pmix.pmix_cpuset_to_string({"source": "", "cpus": [0]}))

    def test_roundtrip(self):
        cpus = {"source": "hwloc", "cpus": [0, 1, 2, 8]}
        self.assertEqual(
            pmix.pmix_cpuset_from_string(pmix.pmix_cpuset_to_string(cpus)), cpus)


class TestCpusetMethods(unittest.TestCase):
    """The cpuset methods were once unconditional PMIX_ERR_NOT_SUPPORTED
    stubs. They now do real work, so before init() they must report a status
    that reflects the actual attempt - never NOT_SUPPORTED."""

    def setUp(self):
        self.s = pmix.PMIxServer()

    def test_methods_present(self):
        for name in ("parse_cpuset_string", "get_cpuset", "compute_distances",
                     "generate_cpuset_string", "generate_locality_string"):
            self.assertTrue(hasattr(pmix.PMIxServer, name),
                            "PMIxServer.%s missing" % name)

    def test_bad_input_is_bad_param(self):
        # These reach the binding's own validation before any C call, so they
        # are meaningful with no init().
        rc, val = self.s.parse_cpuset_string(None)
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        rc, val = self.s.generate_cpuset_string(None)
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        self.assertIsNone(val)
        rc, val = self.s.generate_cpuset_string({"source": "", "cpus": [0]})
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)

    def test_no_longer_stubbed(self):
        # A well-formed cpuset must get past the binding and into the library,
        # which - not being initialized - reports PMIX_ERR_INIT. The old stubs
        # returned PMIX_ERR_NOT_SUPPORTED no matter what was passed.
        good = {"source": "hwloc", "cpus": [0, 1]}
        rc, val = self.s.generate_cpuset_string(good)
        self.assertNotEqual(rc, pmix.PMIX_ERR_NOT_SUPPORTED)
        rc, val = self.s.parse_cpuset_string("hwloc:0-1")
        self.assertNotEqual(rc, pmix.PMIX_ERR_NOT_SUPPORTED)


class TestMethodBinding(unittest.TestCase):
    """Regression tests for methods that were previously unusable because they
    were declared without a `self` parameter (or were truncated). These reach
    an early return before any C call, so they run with no server."""

    def test_server_methods_have_self(self):
        # Every method that was missing self must now declare it as the first
        # parameter. Verify via signature introspection where possible.
        import inspect
        names = ["register_resources", "deregister_resources",
                 "register_attributes", "collect_inventory",
                 "deliver_inventory", "iof_deliver", "define_process_set",
                 "delete_process_set", "session_control"]
        for name in names:
            meth = getattr(pmix.PMIxServer, name, None)
            self.assertIsNotNone(meth, "PMIxServer.%s missing" % name)


class TestNewlyBoundAPIs(unittest.TestCase):
    """The client/server/scheduler APIs and the string converters bound in
    the coverage pass that closed out the last of the unbound C entry points.

    Without a server none of these can complete, but each one converts its
    arguments before handing them to the library and has to release what it
    converted when the library refuses the request (PMIX_ERR_INIT) - the
    same error-path cleanup the non-blocking group tests below cover.  The
    binding's own validation (PMIX_ERR_BAD_PARAM) is fully testable here.
    """

    def setUp(self):
        self.c = pmix.PMIxClient()
        self.s = pmix.PMIxServer()

    def test_methods_present(self):
        for name in ("heartbeat", "progress_thread_stop", "resource_block",
                     "error_code", "value_comparison_string",
                     "group_operation_string",
                     "resource_block_directive_string",
                     "alloc_inheritance_string", "data_print"):
            self.assertTrue(hasattr(pmix.PMIxClient, name),
                            "PMIxClient.%s missing" % name)
        for name in ("generate_regex2", "parse_regex2", "generate_cpuset",
                     "collect_job_info"):
            self.assertTrue(hasattr(pmix.PMIxServer, name),
                            "PMIxServer.%s missing" % name)

    def test_methods_have_self(self):
        # a cdef class method declared without an explicit self shifts its
        # arguments and raises TypeError when called on an instance
        import inspect
        cases = [(pmix.PMIxClient, n) for n in
                 ("heartbeat", "progress_thread_stop", "resource_block",
                  "error_code", "data_print")]
        cases += [(pmix.PMIxServer, n) for n in
                  ("generate_regex2", "parse_regex2", "generate_cpuset",
                   "collect_job_info")]
        for cls, name in cases:
            meth = getattr(cls, name)
            try:
                params = list(inspect.signature(meth).parameters)
            except (TypeError, ValueError):
                continue
            self.assertEqual(params[0], "self",
                             "%s does not declare self first" % name)

    def test_heartbeat_before_init(self):
        # nothing to send to yet, but it must neither raise nor crash
        self.assertEqual(self.c.heartbeat(), pmix.PMIX_SUCCESS)

    def test_progress_thread_stop_before_init(self):
        # no progress thread has been started, so this is a no-op - it must
        # not hang or crash on the unbuilt tracking list
        self.assertEqual(self.c.progress_thread_stop([]), pmix.PMIX_SUCCESS)
        self.assertEqual(
            self.c.progress_thread_stop(
                [{'key': pmix.PMIX_PROGRESS_THREAD_FLUSH, 'value': True,
                  'val_type': pmix.PMIX_BOOL}]),
            pmix.PMIX_SUCCESS)

    def test_resource_block_validates_block_name(self):
        rc = self.c.resource_block(pmix.PMIX_RESOURCE_BLOCK_DEFINE, None,
                                   [], [])
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)

    def test_resource_block_before_init(self):
        # repeat enough times that a leak or double free in the unit/info
        # conversion would be obvious
        units = [{'type': pmix.PMIX_DEVTYPE_GPU, 'count': 4},
                 {'type': pmix.PMIX_DEVTYPE_NETWORK, 'count': 1}]
        info = [{'key': pmix.PMIX_TIMEOUT, 'value': 10,
                 'val_type': pmix.PMIX_INT}]
        for _ in range(64):
            rc = self.c.resource_block(pmix.PMIX_RESOURCE_BLOCK_DEFINE,
                                       "myblock", units, info)
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
        # a missing unit list is legal - the directive may not need one
        self.assertEqual(
            self.c.resource_block(pmix.PMIX_RESOURCE_BLOCK_DELETE, "myblock",
                                  None, None),
            pmix.PMIX_ERR_INIT)

    def test_resource_block_rejects_bad_units(self):
        for bad in ([{'type': "gpu", 'count': 1}],
                    [{'type': pmix.PMIX_DEVTYPE_GPU, 'count': "many"}],
                    ["not-a-dict"]):
            rc = self.c.resource_block(pmix.PMIX_RESOURCE_BLOCK_DEFINE,
                                       "myblock", bad, [])
            self.assertIn(rc, (pmix.PMIX_ERR_BAD_PARAM,
                               pmix.PMIX_ERR_TYPE_MISMATCH),
                          "bad resource unit returned %d" % rc)

    def test_data_print_validates_input(self):
        rc, txt = self.c.data_print(None, "not-a-dict")
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        self.assertIsNone(txt)
        # only a value or an info can be built from a Python dict
        rc, txt = self.c.data_print(None, {'value': 1,
                                           'val_type': pmix.PMIX_INT},
                                    pmix.PMIX_PROC)
        self.assertEqual(rc, pmix.PMIX_ERR_NOT_SUPPORTED)
        # PMIX_INFO was asked for but there is no key to build it from
        rc, txt = self.c.data_print(None, {'value': 1,
                                           'val_type': pmix.PMIX_INT},
                                    pmix.PMIX_INFO)
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)

    def test_data_print_before_init(self):
        # the library refuses before init; the conversion still has to run
        # and be released
        for _ in range(64):
            rc, txt = self.c.data_print("X: ", {'value': 'hello',
                                                'val_type': pmix.PMIX_STRING})
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
            rc, txt = self.c.data_print(None, {'key': pmix.PMIX_UNIV_SIZE,
                                               'value': 16,
                                               'val_type': pmix.PMIX_UINT32})
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_generate_regex2_validates_input(self):
        rc, rgx = self.s.generate_regex2(None)
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        self.assertIsNone(rgx)

    def test_generate_regex2_before_init(self):
        for _ in range(64):
            rc, rgx = self.s.generate_regex2(["n1", "n2", "n3"])
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_parse_regex2_validates_input(self):
        # not a dict, no type, and no bytes are all bad input - and each
        # must be reported rather than reaching the library
        for bad in (None, "notadict", {}, {'type': 'raw'},
                    {'bytes': b'n1,n2'}):
            rc, names = self.s.parse_regex2(bad)
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "parse_regex2(%s) returned %d" % (bad, rc))
            self.assertEqual(names, [])

    def test_parse_regex2_before_init(self):
        rgx = {'type': 'raw', 'bytes': b'n1,n2,n3'}
        for _ in range(64):
            rc, names = self.s.parse_regex2(rgx)
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
            self.assertEqual(names, [])
        # an explicit length shorter than the buffer is honored, and one
        # longer than it is clamped rather than read past the end
        for ln in (0, 3, 8, 99):
            rc, names = self.s.parse_regex2({'type': 'raw',
                                             'bytes': b'n1,n2,n3', 'len': ln})
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_generate_cpuset_validates_input(self):
        rc, cpus = self.s.generate_cpuset(None)
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(cpus, {})

    def test_generate_cpuset_before_init(self):
        rc, cpus = self.s.generate_cpuset("hwloc:0-3,8")
        self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_collect_job_info_requires_procs(self):
        # with no procs there is no job to collect, and the library would
        # walk a NULL namespace list
        for bad in (None, []):
            rc, blob = self.s.collect_job_info(bad)
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
            self.assertEqual(blob['size'], 0)
            self.assertEqual(blob['bytes'], b'')

    def test_collect_job_info_before_init(self):
        procs = [{'nspace': "testnspace", 'rank': 0},
                 {'nspace': "testnspace", 'rank': 1}]
        for _ in range(64):
            rc, blob = self.s.collect_job_info(procs)
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
            self.assertEqual(blob['size'], 0)

    def test_scheduler_role_operations(self):
        # section 1.5: the scheduler drives resource blocks and sessions
        # through what it inherits - both must be reachable on the class
        sched = pmix.PMIxScheduler()
        self.assertTrue(hasattr(sched, "resource_block"))
        self.assertTrue(hasattr(sched, "session_control"))
        # assigning a session to the RTE is a session_control directive -
        # there must be no separate method for it
        self.assertFalse(hasattr(sched, "assign_session"))
        self.assertEqual(
            sched.resource_block(pmix.PMIX_RESOURCE_BLOCK_DEFINE, None,
                                 [], []),
            pmix.PMIX_ERR_BAD_PARAM)


class TestGroupNonBlocking(unittest.TestCase):
    """The non-blocking group bindings.

    Without a server we cannot complete a group operation, but every one of
    these calls marshals its arguments into a keepalive caddy before handing
    the request to the library.  Called before init() the library rejects the
    request with PMIX_ERR_INIT, which is exactly the path that has to unwind
    that caddy by hand (no callback is executed, so nothing else will).  These
    tests therefore cover the argument conversion and the error-path cleanup.
    """

    #: name, args to place before the callback pair
    CASES = (
        ("group_construct_nb", ("mygroup", None, [])),
        ("group_invite_nb", ("mygroup", None, [])),
        ("group_join_nb", ("mygroup", None, 0, [])),
        ("group_leave_nb", ("mygroup", [])),
        ("group_destruct_nb", ("mygroup", [])),
    )

    def setUp(self):
        self.client = pmix.PMIxClient()
        self.fired = []

    def _cb(self, *args):
        self.fired.append(args)

    def test_methods_present(self):
        for name, _ in self.CASES:
            self.assertTrue(hasattr(pmix.PMIxClient, name),
                            "PMIxClient.%s missing" % name)

    def test_methods_have_self(self):
        # A cdef class method declared without an explicit self raises
        # TypeError when called on an instance - the defect that made
        # several older bindings unusable.
        import inspect
        for name, _ in self.CASES:
            meth = getattr(pmix.PMIxClient, name)
            try:
                params = list(inspect.signature(meth).parameters)
            except (TypeError, ValueError):
                # Cython may not expose a signature; the call below in
                # test_error_before_init covers it either way
                continue
            self.assertEqual(params[0], "self",
                             "%s does not declare self first" % name)

    def test_rejects_non_callable(self):
        # A NULL C callback would strand the caddy forever, so a callback
        # that cannot be executed must be refused up front
        for name, args in self.CASES:
            meth = getattr(self.client, name)
            rc = meth(*(args + (None,)))
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "%s accepted a non-callable callback" % name)
            rc = meth(*(args + ("not-a-function",)))
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "%s accepted a non-callable callback" % name)

    def test_error_before_init(self):
        # The library rejects the request, so the callback must NOT fire
        # and the binding must reclaim the caddy itself
        for name, args in self.CASES:
            meth = getattr(self.client, name)
            rc = meth(*(args + (self._cb,)))
            self.assertEqual(rc, pmix.PMIX_ERR_INIT,
                             "%s returned %s" % (name, rc))
        self.assertEqual(self.fired, [],
                         "a callback fired for a request the library refused")

    def test_error_before_init_with_info(self):
        # Same path, but with a real info array and proc list to convert so
        # the caddy actually owns allocations that have to be released
        peers = [{'nspace': "testnspace", 'rank': 0},
                 {'nspace': "testnspace", 'rank': 1}]
        pyinfo = [{'key': pmix.PMIX_GROUP_ASSIGN_CONTEXT_ID,
                   'value': True, 'val_type': pmix.PMIX_BOOL},
                  {'key': pmix.PMIX_TIMEOUT,
                   'value': 10, 'val_type': pmix.PMIX_INT}]
        # repeat enough times that a leak or double free would be obvious
        for _ in range(64):
            rc = self.client.group_construct_nb("mygroup", peers, pyinfo,
                                                self._cb, "opaque")
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
            rc = self.client.group_join_nb("mygroup", peers[0],
                                           pmix.PMIX_GROUP_ACCEPT, pyinfo,
                                           self._cb, "opaque")
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
            rc = self.client.group_destruct_nb("mygroup", pyinfo,
                                               self._cb, "opaque")
            self.assertEqual(rc, pmix.PMIX_ERR_INIT)
        self.assertEqual(self.fired, [])

    def test_no_operations_left_registered(self):
        # Every refused request must have removed itself from the module's
        # in-flight registry - otherwise the callback objects leak
        self.client.group_leave_nb("mygroup", [], self._cb)
        self.assertEqual(len(pmix.pynbcbs), 0,
                         "refused operations left entries in the registry")


class TestClientNonBlocking(unittest.TestCase):
    """The remaining non-blocking client bindings.

    Same reasoning as TestGroupNonBlocking above: without a server none of
    these can complete, but each one converts a full set of arguments into a
    keepalive caddy before handing the request to the library, and the
    library's pre-init refusal is precisely the path that has to unwind that
    caddy by hand.  Running every case therefore covers the argument
    marshaling and the error-path cleanup for all of them.
    """

    PEERS = ({'nspace': "testnspace", 'rank': 0},
             {'nspace': "testnspace", 'rank': 1})
    INFO = ({'key': "pmix.timeout", 'value': 10, 'val_type': 5},)
    APPS = ({'cmd': "/bin/true", 'argv': ["true"], 'env': ["FOO=bar"],
             'maxprocs': 1, 'info': []},)
    QUERIES = ({'keys': ["pmix.qry.nslist"], 'qualifiers': None},)
    UNITS = ({'type': 1, 'count': 2},)
    CRED = {'bytes': b"somecredential"}
    CPUS = {'source': "hwloc", 'cpus': [0, 1]}

    #: name, args to place before the (cbfunc, cbdata) pair
    CASES = (
        ("fence_nb", (list(PEERS), list(INFO))),
        ("get_nb", (PEERS[1], "mykey", list(INFO))),
        # a None proc and a None key are both legal - "me" and "everything"
        ("get_nb", (None, None, None)),
        ("publish_nb", (list(INFO),)),
        ("lookup_nb", (["key1", "key2"], list(INFO))),
        ("unpublish_nb", (["key1"], list(INFO))),
        # a None key list means "everything I published"
        ("unpublish_nb", (None, None)),
        ("spawn_nb", (list(INFO), list(APPS))),
        ("connect_nb", (list(PEERS), list(INFO))),
        ("disconnect_nb", (list(PEERS), list(INFO))),
        ("query_nb", (list(QUERIES),)),
        ("log_nb", (list(INFO), list(INFO))),
        ("allocation_request_nb", (pmix.PMIX_ALLOC_NEW, list(INFO))),
        ("job_control_nb", (list(PEERS), list(INFO))),
        ("monitor_nb", (list(INFO), pmix.PMIX_ERR_TIMEOUT, list(INFO))),
        ("get_credential_nb", (list(INFO),)),
        ("validate_credential_nb", (CRED, list(INFO))),
        ("fabric_register_nb", (list(INFO),)),
        ("fabric_update_nb", ()),
        ("fabric_deregister_nb", ()),
        ("resource_block_nb", (pmix.PMIX_RESOURCE_BLOCK_DEFINE, "myblock",
                               list(UNITS), list(INFO))),
    )

    #: these need a topology, which not every environment can supply, so
    #: they are checked for "refused cleanly" rather than a specific status
    LENIENT = ("compute_distances_nb", (CPUS, list(INFO)))

    def setUp(self):
        self.client = pmix.PMIxClient()
        self.fired = []

    def _cb(self, *args):
        self.fired.append(args)

    def test_methods_present(self):
        for name, _ in self.CASES + (self.LENIENT,):
            self.assertTrue(hasattr(pmix.PMIxClient, name),
                            "PMIxClient.%s missing" % name)

    def test_rejects_non_callable(self):
        # A NULL C callback would strand the caddy forever, so a callback
        # that cannot be executed must be refused up front
        for name, args in self.CASES + (self.LENIENT,):
            meth = getattr(self.client, name)
            rc = meth(*(args + (None,)))
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "%s accepted a non-callable callback" % name)
            rc = meth(*(args + ("not-a-function",)))
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "%s accepted a non-callable callback" % name)

    def test_error_before_init(self):
        # The library rejects the request, so the callback must NOT fire
        # and the binding must reclaim the caddy itself
        for name, args in self.CASES:
            meth = getattr(self.client, name)
            rc = meth(*(args + (self._cb, "opaque")))
            self.assertEqual(rc, pmix.PMIX_ERR_INIT,
                             "%s returned %s" % (name, rc))
        name, args = self.LENIENT
        rc = getattr(self.client, name)(*(args + (self._cb, "opaque")))
        self.assertNotEqual(rc, pmix.PMIX_SUCCESS,
                            "%s was accepted before init" % name)
        self.assertEqual(self.fired, [],
                         "a callback fired for a request the library refused")

    def test_repeated_requests_do_not_accumulate(self):
        # Repeat the whole set enough times that a leak, a double free or a
        # stranded registry entry would show up
        for _ in range(32):
            for name, args in self.CASES + (self.LENIENT,):
                getattr(self.client, name)(*(args + (self._cb, "opaque")))
        self.assertEqual(self.fired, [])
        self.assertEqual(len(pmix.pynbcbs), 0,
                         "refused operations left entries in the registry")

    def test_bad_arguments_rejected(self):
        # Arguments the binding itself has to police, before anything is
        # handed to the library
        self.assertEqual(
            self.client.spawn_nb([], None, self._cb),
            pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.spawn_nb([], [], self._cb),
            pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.validate_credential_nb(None, [], self._cb),
            pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.validate_credential_nb({}, [], self._cb),
            pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.resource_block_nb(pmix.PMIX_RESOURCE_BLOCK_DEFINE,
                                          None, [], [], self._cb),
            pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(self.fired, [])
        self.assertEqual(len(pmix.pynbcbs), 0)

    def test_fabric_ops_require_registration(self):
        # update/deregister are meaningless until a fabric is registered,
        # and the binding says so rather than handing the library a fabric
        # object it never saw
        self.assertEqual(self.client.fabric_update_nb(self._cb),
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.fabric_deregister_nb(self._cb),
                         pmix.PMIX_ERR_INIT)


class TestStructPrinters(unittest.TestCase):
    """The struct pretty-printers.

    Unlike the *_string converters, which translate one enumerated value,
    these render a whole struct.  They touch no library state, so - like the
    converters - they are fully testable without a server.
    """

    VALUE = {'value': 42, 'val_type': pmix.PMIX_INT32}
    INFO = {'key': "pmix.job.size", 'value': 4, 'val_type': pmix.PMIX_UINT32}
    PROC = {'nspace': "testnspace", 'rank': 3}
    APP = {'cmd': "/bin/true", 'argv': ["true", "-x"], 'env': ["FOO=bar"],
           'maxprocs': 2, 'info': []}
    UNIT = {'type': pmix.PMIX_DEVTYPE_GPU, 'count': 4}

    #: name, argument, a substring the rendering must contain
    CASES = (
        ("value_string", VALUE, "42"),
        ("info_string", INFO, "pmix.job.size"),
        ("proc_string", PROC, "testnspace"),
        ("app_string", APP, "/bin/true"),
        ("resource_unit_string", UNIT, "4"),
    )

    def setUp(self):
        self.client = pmix.PMIxClient()

    def test_renders_the_struct(self):
        for name, arg, needle in self.CASES:
            rc, txt = getattr(self.client, name)(arg)
            self.assertEqual(rc, pmix.PMIX_SUCCESS, "%s returned %s" % (name, rc))
            self.assertIsInstance(txt, str, "%s did not return a str" % name)
            self.assertIn(needle, txt, "%s rendered %r" % (name, txt))

    def test_rejects_non_dict(self):
        for name, _, _ in self.CASES:
            for bad in (None, "not-a-dict", 7, []):
                rc, txt = getattr(self.client, name)(bad)
                self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                                 "%s accepted %r" % (name, bad))
                self.assertIsNone(txt)

    def test_info_requires_a_key(self):
        # an info without a key is a value, not an info
        rc, txt = self.client.info_string({'value': 1,
                                           'val_type': pmix.PMIX_INT32})
        self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM)
        self.assertIsNone(txt)

    def test_app_accepts_bytes_for_its_strings(self):
        # the loader ran str() over the command, so a bytes command became
        # the repr "b'/bin/true'" - not a program anything can run.  These
        # bindings take str or bytes everywhere else, and callers do hand
        # back what an upcall gave them.
        rc, txt = self.client.app_string({'cmd': b"/bin/true",
                                          'argv': [b"true"], 'maxprocs': 1})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertIn("/bin/true", txt)
        self.assertNotIn("b'", txt)

    def test_app_with_only_a_command(self):
        # argv, env, cwd and info are all optional; argv defaults to the
        # command itself, as execvp would need
        rc, txt = self.client.app_string({'cmd': "/bin/true"})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertIn("/bin/true", txt)

    def test_app_with_empty_lists(self):
        rc, txt = self.client.app_string({'cmd': "/bin/true", 'argv': [],
                                          'env': [], 'info': [],
                                          'maxprocs': 1})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertIn("/bin/true", txt)

    def test_repeated_calls_are_stable(self):
        # each of these builds a struct, renders it, and has to release
        # both the struct and the string the library allocated
        for _ in range(200):
            for name, arg, _ in self.CASES:
                rc, txt = getattr(self.client, name)(arg)
                self.assertEqual(rc, pmix.PMIX_SUCCESS)


class TestValueConversion(unittest.TestCase):
    """The value load/unload conversion layer.

    pmix_load_value and pmix_unload_value are cdef and so unreachable from
    Python; pmix_value_roundtrip is the hook that exposes them, loading a
    value dict into a pmix_value_t, converting it back, and releasing the
    value.  Nothing here needs a live library.

    The cases come from the old bindings/python/tests/cython smoke test,
    which could only print what it produced - so a wrong struct member or a
    freed-twice pointer looked exactly like a correct conversion.  Here the
    result is checked.
    """

    #: name, input value dict, dict it must convert back to
    SCALARS = (
        # the loader takes the string spelling of a bool; what comes back
        # is the canonical Python one
        ("bool-str", {'value': 'True', 'val_type': pmix.PMIX_BOOL},
         {'value': True, 'val_type': pmix.PMIX_BOOL}),
        ("bool", {'value': False, 'val_type': pmix.PMIX_BOOL}, None),
        ("byte", {'value': 1, 'val_type': pmix.PMIX_BYTE}, None),
        ("string", {'value': "foo", 'val_type': pmix.PMIX_STRING}, None),
        # a PMIX_STRING is allowed to carry a NULL
        ("string-none", {'value': None, 'val_type': pmix.PMIX_STRING}, None),
        ("size", {'value': 45, 'val_type': pmix.PMIX_SIZE}, None),
        ("pid", {'value': 3, 'val_type': pmix.PMIX_PID}, None),
        ("int", {'value': 11, 'val_type': pmix.PMIX_INT}, None),
        ("int8", {'value': 2, 'val_type': pmix.PMIX_INT8}, None),
        ("int16", {'value': 127, 'val_type': pmix.PMIX_INT16}, None),
        ("int32", {'value': 100, 'val_type': pmix.PMIX_INT32}, None),
        ("int64", {'value': 500, 'val_type': pmix.PMIX_INT64}, None),
        ("uint", {'value': 250, 'val_type': pmix.PMIX_UINT}, None),
        ("uint8", {'value': 201, 'val_type': pmix.PMIX_UINT8}, None),
        ("uint16", {'value': 700, 'val_type': pmix.PMIX_UINT16}, None),
        ("uint32", {'value': 301, 'val_type': pmix.PMIX_UINT32}, None),
        ("uint64", {'value': 301, 'val_type': pmix.PMIX_UINT64}, None),
        ("double", {'value': 201.25, 'val_type': pmix.PMIX_DOUBLE}, None),
        ("timeval", {'value': {'sec': 2, 'usec': 3},
                     'val_type': pmix.PMIX_TIMEVAL}, None),
        ("time", {'value': 100, 'val_type': pmix.PMIX_TIME}, None),
        ("status", {'value': -34, 'val_type': pmix.PMIX_STATUS}, None),
        ("rank", {'value': 15, 'val_type': pmix.PMIX_PROC_RANK}, None),
        ("proc", {'value': {'nspace': "testnspace", 'rank': 0},
                  'val_type': pmix.PMIX_PROC}, None),
        ("byte-object", {'value': {'bytes': b"\x01\x02\xff", 'size': 3},
                         'val_type': pmix.PMIX_BYTE_OBJECT}, None),
        ("persist", {'value': 18, 'val_type': pmix.PMIX_PERSIST}, None),
        ("scope", {'value': 1, 'val_type': pmix.PMIX_SCOPE}, None),
        ("range", {'value': 5, 'val_type': pmix.PMIX_DATA_RANGE}, None),
        ("proc-state", {'value': 45, 'val_type': pmix.PMIX_PROC_STATE}, None),
        ("proc-info", {'value': {'proc': {'nspace': "fakenspace", 'rank': 0},
                                 'hostname': "myhostname",
                                 'executable': "testexec",
                                 'pid': 2, 'exitcode': 0, 'state': 0},
                       'val_type': pmix.PMIX_PROC_INFO}, None),
        ("alloc-directive", {'value': 19,
                             'val_type': pmix.PMIX_ALLOC_DIRECTIVE}, None),
        ("envar", {'value': {'envar': "TEST_ENVAR", 'value': "TEST_VAL",
                             'separator': ':'},
                   'val_type': pmix.PMIX_ENVAR}, None),
    )

    def _check(self, name, given, expect):
        rc, got = pmix.pmix_value_roundtrip(given)
        self.assertEqual(rc, pmix.PMIX_SUCCESS,
                         "%s failed to convert: %s" % (name, rc))
        self.assertEqual(got, expect if expect is not None else given,
                         "%s came back as %r" % (name, got))
        return got

    def test_scalar_round_trip(self):
        for name, given, expect in self.SCALARS:
            self._check(name, given, expect)

    def test_float_round_trip(self):
        # a PMIX_FLOAT is single precision, so this one cannot be exact
        rc, got = pmix.pmix_value_roundtrip({'value': 301.1,
                                             'val_type': pmix.PMIX_FLOAT})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['val_type'], pmix.PMIX_FLOAT)
        self.assertAlmostEqual(got['value'], 301.1, places=4)

    def test_output_can_be_fed_back_in(self):
        # what comes out must be loadable again, unchanged - a converter
        # that renders a member in a shape the loader will not take (an
        # envar separator as its ordinal, say) breaks any handler that
        # echoes back what it was given
        for name, given, expect in self.SCALARS:
            once = self._check(name, given, expect)
            rc, twice = pmix.pmix_value_roundtrip(once)
            self.assertEqual(rc, pmix.PMIX_SUCCESS,
                             "%s did not reload: %s" % (name, rc))
            self.assertEqual(twice, once, "%s is not stable" % name)

    def test_bad_arguments_rejected(self):
        for bad in (None, "not-a-dict", 7, [], {}, {'value': 1},
                    {'val_type': pmix.PMIX_INT}):
            rc, got = pmix.pmix_value_roundtrip(bad)
            self.assertEqual(rc, pmix.PMIX_ERR_BAD_PARAM,
                             "accepted %r" % (bad,))
            self.assertIsNone(got)

    def test_repeated_calls_are_stable(self):
        # every one of these allocates, and the value has to be released
        # again on each pass
        for _ in range(200):
            for name, given, expect in self.SCALARS:
                rc, _got = pmix.pmix_value_roundtrip(given)
                self.assertEqual(rc, pmix.PMIX_SUCCESS)


class TestDataArrayConversion(unittest.TestCase):
    """PMIX_DATA_ARRAY, the conversion arm with the most places to go wrong.

    Cython converts a C struct to a Python dict on the fly, so a misspelled
    struct member in one of these arms compiles cleanly and only misbehaves
    at run time (see the bindings AGENTS.md).  Every element type the loader
    claims to support is round-tripped here.
    """

    INFO_ARRAY = {'type': pmix.PMIX_INFO,
                  'array': [{'key': "pmix.alloc.netid",
                             'value': "SIMPSCHED.net",
                             'val_type': pmix.PMIX_STRING},
                            {'key': "pmix.alloc.nsec", 'value': 'T',
                             'val_type': pmix.PMIX_BOOL}]}
    # what the info array above converts back to: an unload always reports
    # the info flags, and the bool arrives canonicalized
    INFO_ARRAY_OUT = {'type': pmix.PMIX_INFO,
                      'array': [{'key': "pmix.alloc.netid", 'flags': 0,
                                 'value': "SIMPSCHED.net",
                                 'val_type': pmix.PMIX_STRING},
                                {'key': "pmix.alloc.nsec", 'flags': 0,
                                 'value': True,
                                 'val_type': pmix.PMIX_BOOL}]}

    #: name, array dict, the array dict it must convert back to
    ARRAYS = (
        ("info", INFO_ARRAY, INFO_ARRAY_OUT),
        # an array nested inside an info inside an array
        ("nested",
         {'type': pmix.PMIX_INFO,
          'array': [{'key': "foo", 'value': INFO_ARRAY,
                     'val_type': pmix.PMIX_DATA_ARRAY}]},
         {'type': pmix.PMIX_INFO,
          'array': [{'key': "foo", 'flags': 0, 'value': INFO_ARRAY_OUT,
                     'val_type': pmix.PMIX_DATA_ARRAY}]}),
        # an array whose *element type* is itself PMIX_DATA_ARRAY - each
        # element is a complete array, not a value wrapping one
        ("darray-of-darrays",
         {'type': pmix.PMIX_DATA_ARRAY,
          'array': [{'type': pmix.PMIX_INT, 'array': [1, 2, 3]},
                    {'type': pmix.PMIX_STRING, 'array': ["abc", "def"]}]},
         None),
        # nesting to a second level, and an element array carrying infos
        ("darray-of-darrays-deep",
         {'type': pmix.PMIX_DATA_ARRAY,
          'array': [INFO_ARRAY,
                    {'type': pmix.PMIX_DATA_ARRAY,
                     'array': [{'type': pmix.PMIX_SIZE, 'array': [7, 8]}]}]},
         {'type': pmix.PMIX_DATA_ARRAY,
          'array': [INFO_ARRAY_OUT,
                    {'type': pmix.PMIX_DATA_ARRAY,
                     'array': [{'type': pmix.PMIX_SIZE, 'array': [7, 8]}]}]}),
        ("string", {'type': pmix.PMIX_STRING,
                    'array': ["abc", "def", "efg"]}, None),
        ("bool", {'type': pmix.PMIX_BOOL, 'array': ['False', 'True']},
         {'type': pmix.PMIX_BOOL, 'array': [False, True]}),
        ("byte", {'type': pmix.PMIX_BYTE, 'array': [1, 2, 255]}, None),
        ("size", {'type': pmix.PMIX_SIZE, 'array': [45, 46, 47]}, None),
        ("pid", {'type': pmix.PMIX_PID, 'array': [3, 4, 5]}, None),
        ("int", {'type': pmix.PMIX_INT, 'array': [1, 2, 3]}, None),
        ("int8", {'type': pmix.PMIX_INT8, 'array': [1, 2]}, None),
        ("int16", {'type': pmix.PMIX_INT16, 'array': [300, 400]}, None),
        ("int32", {'type': pmix.PMIX_INT32, 'array': [-1, 2]}, None),
        ("int64", {'type': pmix.PMIX_INT64, 'array': [1 << 40, 2]}, None),
        ("uint32", {'type': pmix.PMIX_UINT32, 'array': [7, 8]}, None),
        ("uint64", {'type': pmix.PMIX_UINT64, 'array': [7, 8]}, None),
        ("float", {'type': pmix.PMIX_FLOAT, 'array': [1.5, 2.5]}, None),
        ("double", {'type': pmix.PMIX_DOUBLE, 'array': [1.25, 2.5]}, None),
        ("timeval", {'type': pmix.PMIX_TIMEVAL,
                     'array': [{'sec': 2, 'usec': 3},
                               {'sec': 1, 'usec': 2}]}, None),
        ("time", {'type': pmix.PMIX_TIME, 'array': [100, 200]}, None),
        ("status", {'type': pmix.PMIX_STATUS, 'array': [-1, -2]}, None),
        ("rank", {'type': pmix.PMIX_PROC_RANK, 'array': [0, 1]}, None),
        ("proc", {'type': pmix.PMIX_PROC,
                  'array': [{'nspace': "testnspace", 'rank': 0},
                            {'nspace': "newnspace", 'rank': 1}]}, None),
        ("byte-object", {'type': pmix.PMIX_BYTE_OBJECT,
                         'array': [{'bytes': b"\x01", 'size': 1},
                                   {'bytes': b"\x02\xfe", 'size': 2}]}, None),
        ("persist", {'type': pmix.PMIX_PERSIST, 'array': [1, 2]}, None),
        ("scope", {'type': pmix.PMIX_SCOPE, 'array': [1, 2]}, None),
        ("range", {'type': pmix.PMIX_DATA_RANGE, 'array': [1, 2]}, None),
        ("proc-state", {'type': pmix.PMIX_PROC_STATE, 'array': [0, 1]}, None),
        ("alloc-directive", {'type': pmix.PMIX_ALLOC_DIRECTIVE,
                             'array': [1, 2]}, None),
        ("proc-info", {'type': pmix.PMIX_PROC_INFO,
                       'array': [{'proc': {'nspace': "fakenspace", 'rank': 0},
                                  'hostname': "myhostname",
                                  'executable': "testexec",
                                  'pid': 2, 'exitcode': 0,
                                  'state': 0}]}, None),
        ("envar", {'type': pmix.PMIX_ENVAR,
                   'array': [{'envar': "TEST_ENVAR", 'value': "TEST_VAL",
                              'separator': ':'}]}, None),
    )

    @staticmethod
    def _value(array):
        return {'value': array, 'val_type': pmix.PMIX_DATA_ARRAY}

    def test_round_trip(self):
        for name, given, expect in self.ARRAYS:
            rc, got = pmix.pmix_value_roundtrip(self._value(given))
            self.assertEqual(rc, pmix.PMIX_SUCCESS,
                             "%s array failed to convert: %s" % (name, rc))
            self.assertEqual(got, self._value(expect if expect is not None
                                              else given),
                             "%s array came back as %r" % (name, got))

    def test_output_can_be_fed_back_in(self):
        for name, given, expect in self.ARRAYS:
            rc, once = pmix.pmix_value_roundtrip(self._value(given))
            self.assertEqual(rc, pmix.PMIX_SUCCESS)
            rc, twice = pmix.pmix_value_roundtrip(once)
            self.assertEqual(rc, pmix.PMIX_SUCCESS,
                             "%s array did not reload: %s" % (name, rc))
            self.assertEqual(twice, once, "%s array is not stable" % name)

    def test_element_count_preserved(self):
        for name, given, expect in self.ARRAYS:
            rc, got = pmix.pmix_value_roundtrip(self._value(given))
            self.assertEqual(rc, pmix.PMIX_SUCCESS)
            self.assertEqual(len(got['value']['array']),
                             len(given['array']),
                             "%s array changed length" % name)

    def test_empty_array(self):
        rc, got = pmix.pmix_value_roundtrip(
            self._value({'type': pmix.PMIX_STRING, 'array': []}))
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value']['array'], [])

    def test_null_string_element(self):
        # a NULL entry is legal in a string array, as it is in a scalar
        rc, got = pmix.pmix_value_roundtrip(
            self._value({'type': pmix.PMIX_STRING, 'array': ["a", None]}))
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value']['array'], ["a", None])

    def test_unconvertible_array_reports_an_error(self):
        # a failed load has to be reported rather than handed back as a
        # value whose array was never filled in
        bad = (
            # no loader arm for this element type at all
            {'type': pmix.PMIX_COORD, 'array': [1]},
            # right type, wrong element
            {'type': pmix.PMIX_STRING, 'array': ["abc", 42]},
            {'type': pmix.PMIX_BOOL, 'array': ["yes"]},
            {'type': pmix.PMIX_BYTE, 'array': [1, 999]},
        )
        for array in bad:
            rc, got = pmix.pmix_value_roundtrip(self._value(array))
            self.assertNotEqual(rc, pmix.PMIX_SUCCESS,
                                "accepted %r" % (array,))
            self.assertIsNone(got)

    def test_repeated_calls_are_stable(self):
        # each pass allocates the array and every pointer in it, and has to
        # give all of it back
        for _ in range(100):
            for name, given, expect in self.ARRAYS:
                rc, _got = pmix.pmix_value_roundtrip(self._value(given))
                self.assertEqual(rc, pmix.PMIX_SUCCESS)


class TestConversionBoundaries(unittest.TestCase):
    """The edges of the conversion layer: widths, empties, and NULLs.

    Every case here stands for a defect the 2026-08 review found.  They are
    grouped separately from TestValueConversion because what they check is
    not "does this type round-trip" but "does the boundary behave" - the
    largest value a field can hold, the smallest array, an optional string
    the C side left unset.  Those are exactly the inputs a round-trip suite
    built from typical values never reaches.
    """

    #: val_type, largest value the field can hold, first value past it
    WIDTHS = (
        ("uint8", pmix.PMIX_UINT8, 255, 256),
        ("uint16", pmix.PMIX_UINT16, 65535, 65536),
        ("uint32", pmix.PMIX_UINT32, 4294967295, 4294967296),
        ("uint64", pmix.PMIX_UINT64, 18446744073709551615,
         18446744073709551616),
        ("rank", pmix.PMIX_PROC_RANK, 4294967294, 4294967296),
        ("int8", pmix.PMIX_INT8, 127, 128),
        ("int16", pmix.PMIX_INT16, 32767, 32768),
        ("int32", pmix.PMIX_INT32, 2147483647, 2147483648),
        ("int64", pmix.PMIX_INT64, 9223372036854775807,
         9223372036854775808),
    )

    def test_largest_value_is_accepted(self):
        # the bounds checks were written as products (65536*65536) and
        # simply had the wrong value in several arms, so the top of the
        # range was refused - or, worse, one past the top was accepted and
        # silently truncated by the C store
        for name, vtype, top, _over in self.WIDTHS:
            rc, got = pmix.pmix_value_roundtrip({'value': top,
                                                 'val_type': vtype})
            self.assertEqual(rc, pmix.PMIX_SUCCESS,
                             "%s refused its largest value: %s" % (name, rc))
            self.assertEqual(got['value'], top,
                             "%s did not survive the round trip" % name)

    def test_out_of_range_is_refused(self):
        for name, vtype, _top, over in self.WIDTHS:
            rc, got = pmix.pmix_value_roundtrip({'value': over,
                                                 'val_type': vtype})
            self.assertNotEqual(rc, pmix.PMIX_SUCCESS,
                                "%s accepted %d" % (name, over))
            self.assertIsNone(got)

    def test_negative_is_refused_by_unsigned_types(self):
        for vtype in (pmix.PMIX_UINT, pmix.PMIX_UINT8, pmix.PMIX_UINT16,
                      pmix.PMIX_UINT32, pmix.PMIX_UINT64, pmix.PMIX_SIZE,
                      pmix.PMIX_PROC_RANK, pmix.PMIX_BYTE, pmix.PMIX_SCOPE,
                      pmix.PMIX_DATA_RANGE, pmix.PMIX_PERSIST,
                      pmix.PMIX_PROC_STATE):
            rc, got = pmix.pmix_value_roundtrip({'value': -1,
                                                 'val_type': vtype})
            self.assertNotEqual(rc, pmix.PMIX_SUCCESS,
                                "type %d accepted -1" % vtype)
            self.assertIsNone(got)

    def test_empty_array_of_every_type(self):
        # An empty array carries no block at all.  Every unload arm used to
        # test the block for NULL and, finding one, "return PMIX_ERR_NOMEM"
        # - an int, from a function declared to return a dict, which raises
        # TypeError.  So an empty array of any type blew up rather than
        # converting to [].
        for vtype in (pmix.PMIX_INT, pmix.PMIX_STRING, pmix.PMIX_BOOL,
                      pmix.PMIX_BYTE, pmix.PMIX_SIZE, pmix.PMIX_PROC,
                      pmix.PMIX_INFO, pmix.PMIX_BYTE_OBJECT,
                      pmix.PMIX_ENVAR, pmix.PMIX_PROC_INFO,
                      pmix.PMIX_DATA_ARRAY, pmix.PMIX_FLOAT,
                      pmix.PMIX_DOUBLE, pmix.PMIX_PROC_RANK):
            rc, got = pmix.pmix_value_roundtrip(
                {'value': {'type': vtype, 'array': []},
                 'val_type': pmix.PMIX_DATA_ARRAY})
            self.assertEqual(rc, pmix.PMIX_SUCCESS,
                             "empty array of type %d failed: %s" % (vtype, rc))
            self.assertEqual(got['value'], {'type': vtype, 'array': []})

    def test_byte_object_trusts_the_payload_not_the_count(self):
        # a 'size' larger than the bytes actually provided used to be taken
        # at face value and memcpy'd, reading off the end of the object
        rc, got = pmix.pmix_value_roundtrip(
            {'value': {'bytes': b'\x01\x02', 'size': 4096},
             'val_type': pmix.PMIX_BYTE_OBJECT})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], {'bytes': b'\x01\x02', 'size': 2})

    def test_byte_object_keeps_high_bytes_and_nuls(self):
        payload = bytes(range(256))
        rc, got = pmix.pmix_value_roundtrip(
            {'value': {'bytes': payload, 'size': len(payload)},
             'val_type': pmix.PMIX_BYTE_OBJECT})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value']['bytes'], payload)
        self.assertEqual(got['value']['size'], 256)

    def test_empty_byte_object(self):
        rc, got = pmix.pmix_value_roundtrip(
            {'value': {'bytes': b'', 'size': 0},
             'val_type': pmix.PMIX_BYTE_OBJECT})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], {'bytes': b'', 'size': 0})

    #: a coord's vector is the raw uint32 block, carried as bytes
    @staticmethod
    def _coord(*vals):
        return {'view': 1,
                'coord': b''.join(v.to_bytes(4, sys.byteorder) for v in vals),
                'dims': len(vals)}

    def test_coord_round_trip(self):
        given = self._coord(1, 2, 3)
        rc, got = pmix.pmix_value_roundtrip({'value': given,
                                             'val_type': pmix.PMIX_COORD})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], given)

    def test_coord_dims_larger_than_the_vector_is_refused(self):
        # 'dims' counts coordinates, so the payload must be four times as
        # long; a larger dims used to be believed and read past the end
        bad = {'view': 1, 'coord': (1).to_bytes(4, sys.byteorder), 'dims': 64}
        rc, got = pmix.pmix_value_roundtrip({'value': bad,
                                             'val_type': pmix.PMIX_COORD})
        self.assertNotEqual(rc, pmix.PMIX_SUCCESS)
        self.assertIsNone(got)

    def test_empty_coord(self):
        rc, got = pmix.pmix_value_roundtrip(
            {'value': {'view': 0, 'coord': b'', 'dims': 0},
             'val_type': pmix.PMIX_COORD})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value']['dims'], 0)

    def test_geometry_round_trip(self):
        # the geometry loader never set 'ncoords', and the library's
        # destructor walks exactly that many coordinates - so releasing a
        # geometry ran off the end of the block.  It also reported
        # PMIX_ERR_NOT_SUPPORTED after building the whole struct.
        given = {'fabric': 2, 'uuid': "fab-uuid", 'osname': "ib0",
                 'ncoords': 2,
                 'coords': [self._coord(1, 2), self._coord(9)]}
        rc, got = pmix.pmix_value_roundtrip({'value': given,
                                             'val_type': pmix.PMIX_GEOMETRY})
        self.assertEqual(rc, pmix.PMIX_SUCCESS,
                         "geometry failed to convert: %s" % rc)
        self.assertEqual(got['value'], given)

    def test_geometry_with_no_coordinates(self):
        given = {'fabric': 0, 'uuid': "u", 'osname': "o",
                 'ncoords': 0, 'coords': []}
        rc, got = pmix.pmix_value_roundtrip({'value': given,
                                             'val_type': pmix.PMIX_GEOMETRY})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], given)

    def test_endpoint_round_trip(self):
        given = {'uuid': "ep-uuid", 'osname': "eth0",
                 'endpt': {'bytes': b'\x00\xff\x10', 'size': 3}}
        rc, got = pmix.pmix_value_roundtrip({'value': given,
                                             'val_type': pmix.PMIX_ENDPOINT})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], given)

    def test_nested_data_arrays(self):
        given = {'type': pmix.PMIX_DATA_ARRAY, 'array': [
            {'type': pmix.PMIX_INT, 'array': [1, 2, 3]},
            {'type': pmix.PMIX_STRING, 'array': ["a", None]},
            {'type': pmix.PMIX_INT, 'array': []},
        ]}
        rc, got = pmix.pmix_value_roundtrip({'value': given,
                                            'val_type': pmix.PMIX_DATA_ARRAY})
        self.assertEqual(rc, pmix.PMIX_SUCCESS)
        self.assertEqual(got['value'], given)

    def test_boundary_cases_are_stable(self):
        # each of these allocates, and a leak or a double free only shows up
        # when the same conversion is run many times over
        cases = [{'value': top, 'val_type': vtype}
                 for _n, vtype, top, _o in self.WIDTHS]
        cases.append({'value': self._coord(1, 2, 3),
                      'val_type': pmix.PMIX_COORD})
        cases.append({'value': {'fabric': 0, 'uuid': "u", 'osname': "o",
                                'ncoords': 1, 'coords': [self._coord(4)]},
                      'val_type': pmix.PMIX_GEOMETRY})
        cases.append({'value': {'bytes': bytes(range(256)), 'size': 256},
                      'val_type': pmix.PMIX_BYTE_OBJECT})
        cases.append({'value': {'type': pmix.PMIX_INT, 'array': []},
                      'val_type': pmix.PMIX_DATA_ARRAY})
        for _ in range(200):
            for case in cases:
                rc, _got = pmix.pmix_value_roundtrip(case)
                self.assertEqual(rc, pmix.PMIX_SUCCESS)


class TestDataBindings(unittest.TestCase):
    """The serialization family.

    A Python data buffer is a dict, so these need no C object to be created
    or released - but every one of them does need an initialized library, so
    without a server all that can be checked here is that they marshal their
    arguments, refuse cleanly, and release what they built.  The round trip
    itself is exercised in server.py, which has a live library.
    """

    VALUE = {'value': 42, 'val_type': pmix.PMIX_INT32}
    INFO = {'key': "pmix.job.size", 'value': 4, 'val_type': pmix.PMIX_UINT32}
    BUF = {'bytes': b"abcd", 'bytes_used': 4, 'bytes_unpacked': 0}
    PAYLOAD = {'bytes': b"payload", 'size': 7}

    def setUp(self):
        self.client = pmix.PMIxClient()

    def test_methods_present(self):
        for name in ("data_pack", "data_unpack", "data_copy",
                     "data_copy_payload", "data_load", "data_unload",
                     "data_embed", "data_compress", "data_decompress"):
            self.assertTrue(hasattr(pmix.PMIxClient, name),
                            "PMIxClient.%s missing" % name)

    def test_error_before_init(self):
        # the library refuses every one of these until it is initialized;
        # what matters is that they report it rather than crashing
        self.assertEqual(self.client.data_pack(None, self.VALUE)[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_pack(dict(self.BUF), self.INFO)[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_unpack(dict(self.BUF))[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_copy(self.VALUE)[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(
            self.client.data_copy_payload(None, dict(self.BUF))[0],
            pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_unload(dict(self.BUF))[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_load(None, self.PAYLOAD)[0],
                         pmix.PMIX_ERR_INIT)
        self.assertEqual(self.client.data_embed(None, self.PAYLOAD)[0],
                         pmix.PMIX_ERR_INIT)

    def test_bad_arguments_rejected(self):
        # arguments the binding polices itself, before the library is asked
        self.assertEqual(self.client.data_pack(None, None)[0],
                         pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(self.client.data_pack("not-a-buffer", self.VALUE)[0],
                         pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.data_pack(None, self.VALUE, pmix.PMIX_PROC)[0],
            pmix.PMIX_ERR_NOT_SUPPORTED)
        self.assertEqual(self.client.data_unpack(None)[0],
                         pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(
            self.client.data_unpack(dict(self.BUF), pmix.PMIX_PROC)[0],
            pmix.PMIX_ERR_NOT_SUPPORTED)
        self.assertEqual(self.client.data_copy(None)[0],
                         pmix.PMIX_ERR_BAD_PARAM)
        self.assertEqual(self.client.data_unload(None)[0],
                         pmix.PMIX_ERR_BAD_PARAM)
        for bad in (None, b""):
            self.assertEqual(self.client.data_compress(bad)[0],
                             pmix.PMIX_ERR_BAD_PARAM)
            self.assertEqual(self.client.data_decompress(bad)[0],
                             pmix.PMIX_ERR_BAD_PARAM)

    def test_buffer_dict_is_not_corrupted(self):
        # a refused call must leave the caller's buffer readable
        buf = dict(self.BUF)
        self.client.data_pack(buf, self.VALUE)
        self.assertEqual(buf['bytes'], b"abcd")
        self.assertEqual(buf['bytes_unpacked'], 0)

    def test_repeated_calls_are_stable(self):
        for _ in range(200):
            self.client.data_pack(dict(self.BUF), self.VALUE)
            self.client.data_unpack(dict(self.BUF), pmix.PMIX_VALUE)
            self.client.data_load(None, self.PAYLOAD)
            self.client.data_embed(None, self.PAYLOAD)


class TestToolServerModule(unittest.TestCase):
    """PMIxTool.set_server_module.

    The method used to populate its module struct and return PMIX_SUCCESS
    without ever handing it to the library, so a tool's handlers were never
    called.  It now reports what the library says - which, before
    PMIx_tool_init, is that there is no library to give it to.
    """

    @staticmethod
    def _handler(request, cbfunc, cbdata):
        return pmix.PMIX_SUCCESS

    def test_refused_before_init(self):
        # this used to segfault: the C entry point marked our peer as a
        # server without checking that we had one
        tool = pmix.PMIxTool()
        rc = tool.set_server_module({'clientconnected': self._handler})
        self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_empty_map_refused(self):
        tool = pmix.PMIxTool()
        self.assertEqual(tool.set_server_module({}), pmix.PMIX_ERR_INIT)
        self.assertEqual(tool.set_server_module(None), pmix.PMIX_ERR_INIT)


class TestOptionalCallbackForms(unittest.TestCase):
    """The server calls that take an optional completion callback.

    PMIx expresses "non-blocking" two ways: a separate *_nb function (the
    client family), and an optional trailing cbfunc/cbdata on the same
    entry point (mostly the server family).  These bindings used to hard-
    code NULL for the second kind, so the only behavior reachable from
    Python was the blocking one - which cannot be used from a server
    module upcall, because those run on the progress thread and the
    blocking form would there be waiting on the event loop it is standing
    in.

    Only the argument checking is testable without a live server; the
    round trip is exercised in server.py, and from inside an actual
    upcall by test/unit/progress_threads.c.
    """

    def setUp(self):
        # a tool inherits every client and server method, so one object
        # reaches all fifteen - iof_deregister and iof_push are tool-role
        self.server = pmix.PMIxTool()

    #: name -> a call of that method with 'cb' in the callback position
    def _calls(self, cb):
        proc = {'nspace': "testns", 'rank': 0}
        payload = {'bytes': b"hello", 'size': 5}
        return {
            'register_nspace':
                lambda: self.server.register_nspace("testns", 1, [], cb),
            'deregister_nspace':
                lambda: self.server.deregister_nspace("testns", cb),
            'register_client':
                lambda: self.server.register_client(proc, 0, 0, cb),
            'deregister_client':
                lambda: self.server.deregister_client(proc, cb),
            'notify_event':
                lambda: self.server.notify_event(pmix.PMIX_ERR_JOB_TERMINATED,
                                                 proc, pmix.PMIX_RANGE_LOCAL,
                                                 [], cb),
            'register_resources':
                lambda: self.server.register_resources([], cb),
            'deregister_resources':
                lambda: self.server.deregister_resources([], cb),
            'setup_local_support':
                lambda: self.server.setup_local_support("testns", [], cb),
            'deliver_inventory':
                lambda: self.server.deliver_inventory([], [], cb),
            'iof_deliver':
                lambda: self.server.iof_deliver(proc,
                                                pmix.PMIX_FWD_STDOUT_CHANNEL,
                                                payload, [], cb),
            'iof_flow_control':
                lambda: self.server.iof_flow_control(proc,
                                                     pmix.PMIX_FWD_STDIN_CHANNEL,
                                                     True, [], cb),
            'session_control':
                lambda: self.server.session_control(1, [], cb),
            'deregister_event_handler':
                lambda: self.server.deregister_event_handler(0, cb),
            'iof_deregister':
                lambda: self.server.iof_deregister(0, [], cb),
            'iof_push':
                lambda: self.server.iof_push([proc], payload, [], cb),
        }

    def test_every_optional_callback_api_is_covered(self):
        # if a new one is bound, add it here - the whole point of this
        # class is that the list does not drift
        self.assertEqual(len(self._calls(lambda *a: None)), 15)

    def test_non_callable_callback_is_refused(self):
        # a callback that cannot be called would strand the operation's
        # caddy for the life of the process, so it is checked before
        # anything is allocated
        for bad in (42, "not callable", [], {}):
            for name, call in self._calls(bad).items():
                self.assertEqual(call(), pmix.PMIX_ERR_BAD_PARAM,
                                 "%s accepted %r as a callback" % (name, bad))

    def test_accepting_a_callable_does_not_raise(self):
        # before init the library refuses every one of these, but the
        # binding must marshal the arguments and answer a status rather
        # than raise - a shape error here is a TypeError, not an rc
        def cb(*args):
            pass

        for name, call in self._calls(cb).items():
            rc = call()
            self.assertIsInstance(rc, int, "%s did not answer a status" % name)

    def test_blocking_form_is_still_the_default(self):
        # omitting the callback must keep the behavior every existing
        # caller depends on - a status, not a promise of one
        rc = self.server.deregister_nspace("no-such-nspace")
        self.assertIsInstance(rc, int)

    def test_callback_is_optional_positionally_and_by_keyword(self):
        # the parameter was appended, so existing positional calls keep
        # working and the new one can be passed either way
        def cb(status, cbdata):
            pass

        for call in (lambda: self.server.deregister_nspace("n", cb),
                     lambda: self.server.deregister_nspace("n", cbfunc=cb),
                     lambda: self.server.deregister_nspace("n", cb, {'x': 1}),
                     lambda: self.server.deregister_nspace(
                         "n", cbfunc=cb, cbdata={'x': 1})):
            # before init the library refuses, but the binding must accept
            # the shape rather than raise TypeError
            self.assertIsInstance(call(), int)


class TestServerModuleRegistration(unittest.TestCase):
    """setmodulefn, the gate on the server-module map.

    A key it does not recognize is never wired into pmix_server_module_t,
    so accepting one silently means the caller's handler is never called
    and nothing says so.  init() used to guard this with "except KeyError",
    which setmodulefn cannot raise - it returns a status.
    """

    @staticmethod
    def _handler(request, cbfunc, cbdata):
        return pmix.PMIX_SUCCESS

    def test_known_key_is_accepted(self):
        self.assertEqual(pmix.setmodulefn('clientconnected', self._handler),
                         pmix.PMIX_SUCCESS)

    def test_unknown_key_is_refused(self):
        self.assertEqual(pmix.setmodulefn('nosuchhandler', self._handler),
                         pmix.PMIX_ERR_BAD_PARAM)

    def test_non_callable_is_refused(self):
        # a handler that cannot be called would fail inside the upcall, on
        # the library's progress thread, where there is nothing useful to
        # report it to
        for bad in (None, 42, "a string", []):
            self.assertEqual(pmix.setmodulefn('clientfinalized', bad),
                             pmix.PMIX_ERR_BAD_PARAM,
                             "accepted %r as a handler" % (bad,))

    def test_server_init_refuses_an_unknown_key(self):
        server = pmix.PMIxServer()
        rc = server.init([], {'nosuchhandler': self._handler})
        self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_server_init_refuses_a_non_callable(self):
        server = pmix.PMIxServer()
        rc = server.init([], {'clientconnected': "not callable"})
        self.assertEqual(rc, pmix.PMIX_ERR_INIT)

    def test_registration_replaces_a_previous_handler(self):
        # the map is module-global, so a second server (or a re-init) has
        # to be able to change a handler - it used to keep the first one
        def first(request, cbfunc, cbdata):
            return pmix.PMIX_SUCCESS

        def second(request, cbfunc, cbdata):
            return pmix.PMIX_SUCCESS

        pmix.setmodulefn('monitor', first)
        self.assertIs(pmix.pmixservermodule['monitor'], first)
        pmix.setmodulefn('monitor', second)
        self.assertIs(pmix.pmixservermodule['monitor'], second)


if __name__ == "__main__":
    unittest.main(verbosity=2)
