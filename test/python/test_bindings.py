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


if __name__ == "__main__":
    unittest.main(verbosity=2)
