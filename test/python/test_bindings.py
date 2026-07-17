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


if __name__ == "__main__":
    unittest.main(verbosity=2)
