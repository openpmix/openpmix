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

    def test_assign_session_is_bound_and_complete(self):
        # Previously truncated (returned None) and missing self. It must now
        # accept its four args and return a real status.
        sched = pmix.PMIxScheduler()
        rc = sched.assign_session(0, "alloc-0", [], [])
        self.assertEqual(rc, pmix.PMIX_ERR_NOT_SUPPORTED)

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
    """The client/server APIs bound to close the gaps in MISSING_BINDINGS.md
    sections 1.2, 1.3, 1.5 and 1.6.

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

    def test_repeated_calls_are_stable(self):
        # each of these builds a struct, renders it, and has to release
        # both the struct and the string the library allocated
        for _ in range(200):
            for name, arg, _ in self.CASES:
                rc, txt = getattr(self.client, name)(arg)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
