#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Tests for check_compat.py: each case writes an older and a newer
# pmix_common.h.in (and, where it matters, pmix_server.h) and checks
# which differences the script calls breaking.
#
#   python3 contrib/test_check_compat.py

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_compat  # noqa: E402

HAVE_CC = shutil.which(os.environ.get("CC", "cc").split()[0]) is not None

PRELUDE = """
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int pmix_status_t;
"""


class Case(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="check-compat-test-")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def tree(self, name, common, server=None):
        d = os.path.join(self.dir, name)
        os.makedirs(d)
        with open(os.path.join(d, "pmix_common.h.in"), "w") as f:
            f.write(PRELUDE + common)
        with open(os.path.join(d, "pmix.h"), "w") as f:
            f.write("#include <pmix_common.h>\n")
        if server is not None:
            with open(os.path.join(d, "pmix_server.h"), "w") as f:
                f.write("#include <pmix_common.h>\n" + server)
        return d

    def run_check(self, old, new, oldserver=None, newserver=None, compile=False):
        o = self.tree("old", old, oldserver)
        n = self.tree("new", new, newserver)
        args = ["--src", n, "--tgt", o, "--all"] + (["--compile"] if compile else [])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = check_compat.main(args)
        return rc, out.getvalue()

    def assertBreaks(self, old, new, text=None, **kw):
        rc, out = self.run_check(old, new, **kw)
        self.assertEqual(rc, 1, out)
        if text:
            self.assertIn(text, out.split("COMPATIBLE CHANGES")[0])

    def assertCompatible(self, old, new, **kw):
        rc, out = self.run_check(old, new, **kw)
        self.assertEqual(rc, 0, out)
        return out


class Constants(Case):
    def test_same_value_written_differently(self):
        self.assertCompatible('#define PMIX_A 0x01\n#define PMIX_B (pmix_status_t) -3\n',
                              '#define PMIX_A 0x0000000000000001\n#define PMIX_B -3\n')

    def test_value_changed(self):
        self.assertBreaks('#define PMIX_A 1\n', '#define PMIX_A 2\n', "PMIX_A")

    def test_attribute_string_changed(self):
        self.assertBreaks('#define PMIX_FOO "pmix.foo"   // (bool) a flag\n',
                          '#define PMIX_FOO "pmix.foo2"  // (bool) a flag\n', "PMIX_FOO")

    def test_comment_is_not_interface(self):
        self.assertCompatible('#define PMIX_FOO "pmix.foo"   // (bool) a flag\n',
                              '/* new comment */\n#define PMIX_FOO "pmix.foo" // (int) changed\n')

    def test_removed(self):
        self.assertBreaks('#define PMIX_A 1\n#define PMIX_B 2\n', '#define PMIX_A 1\n',
                          "REMOVED: PMIX_B")

    def test_added(self):
        out = self.assertCompatible('#define PMIX_A 1\n', '#define PMIX_A 1\n#define PMIX_B 2\n')
        self.assertIn("PMIX_B", out)

    def test_define_became_enumerator(self):
        self.assertCompatible('#define PMIX_A 3\n',
                              'typedef enum { PMIX_Z = 2, PMIX_A } pmix_e_t;\n')

    def test_enumerator_value_changed(self):
        self.assertBreaks('typedef enum { PMIX_X, PMIX_Y } pmix_e_t;\n',
                          'typedef enum { PMIX_W, PMIX_X, PMIX_Y } pmix_e_t;\n', "PMIX_X")

    def test_initializer_default_changed(self):
        self.assertCompatible('#define PMIX_S_STATIC_INIT { .a = 0 }\n',
                              '#define PMIX_S_STATIC_INIT { .a = 1 }\n')


class Functions(Case):
    def test_parameter_renamed(self):
        self.assertCompatible('PMIX_EXPORT pmix_status_t PMIx_F(const char *key, size_t n);\n',
                              'PMIX_EXPORT pmix_status_t PMIx_F(const char *name,\n'
                              '                                 size_t count);\n')

    def test_array_is_pointer(self):
        self.assertCompatible('PMIX_EXPORT void PMIx_F(int info[], size_t n);\n',
                              'PMIX_EXPORT void PMIx_F(int *info, size_t n);\n')

    def test_const_added(self):
        self.assertCompatible('PMIX_EXPORT void PMIx_F(char *s);\n',
                              'PMIX_EXPORT void PMIx_F(const char *s);\n')

    def test_const_removed(self):
        self.assertBreaks('PMIX_EXPORT void PMIx_F(const char *s);\n',
                          'PMIX_EXPORT void PMIx_F(char *s);\n', "PMIx_F")

    def test_parameter_added(self):
        self.assertBreaks('PMIX_EXPORT void PMIx_F(char *s);\n',
                          'PMIX_EXPORT void PMIx_F(char *s, bool b);\n', "PMIx_F")

    def test_return_type_changed(self):
        self.assertBreaks('PMIX_EXPORT void PMIx_F(void);\n',
                          'PMIX_EXPORT pmix_status_t PMIx_F(void);\n', "PMIx_F")

    def test_macro_became_function(self):
        self.assertCompatible('#define PMIx_F() do { } while (0)\n',
                              'PMIX_EXPORT void PMIx_F(void);\n')

    def test_macro_lost_argument(self):
        self.assertBreaks('#define PMIX_M(a, b) ((a) + (b))\n', '#define PMIX_M(a) (a)\n',
                          "PMIX_M")

    def test_macro_body_changed(self):
        self.assertCompatible('#define PMIX_M(a) \\\n    (a)\n', '#define PMIX_M(x) ((x) + 0)\n')

    def test_callback_type_changed(self):
        self.assertBreaks('typedef void (*pmix_cb_fn_t)(int a, void *cbdata);\n',
                          'typedef void (*pmix_cb_fn_t)(long a, void *cbdata);\n',
                          "pmix_cb_fn_t")

    def test_inline_function_ignored_body(self):
        self.assertCompatible('static inline int pmix_f(int a) { return a; }\n',
                              'static inline int pmix_f(int b)\n{\n    if (b) { return 1; }\n'
                              '    return 0;\n}\n')


class Structures(Case):
    OLD = 'typedef struct { int a; char *b; } pmix_s_t;\n'

    def test_member_appended(self):
        self.assertBreaks(self.OLD, 'typedef struct { int a; char *b; int c; } pmix_s_t;\n',
                          "pmix_s_t")

    def test_member_reordered(self):
        self.assertBreaks(self.OLD, 'typedef struct { char *b; int a; } pmix_s_t;\n',
                          "pmix_s_t")

    def test_extern_c_braces(self):
        self.assertCompatible('extern "C" {\n' + self.OLD + '}\n',
                              self.OLD)

    @unittest.skipUnless(HAVE_CC, "no C compiler")
    def test_union_grew_within_size(self):
        old = 'typedef struct { int t; union { double d; char *s; } data; } pmix_v_t;\n'
        new = 'typedef struct { int t; union { double d; char *s; int i; } data; } pmix_v_t;\n'
        out = self.assertCompatible(old, new, compile=True)
        self.assertIn("layout unchanged", out)

    @unittest.skipUnless(HAVE_CC, "no C compiler")
    def test_union_grew_beyond_size(self):
        old = 'typedef struct { int t; union { double d; char *s; } data; } pmix_v_t;\n'
        new = 'typedef struct { int t; union { double d; char s[64]; } data; } pmix_v_t;\n'
        self.assertBreaks(old, new, "size changed", compile=True)

    def test_union_grew_without_compile(self):
        old = 'typedef struct { int t; union { double d; } data; } pmix_v_t;\n'
        new = 'typedef struct { int t; union { double d; int i; } data; } pmix_v_t;\n'
        self.assertBreaks(old, new, "--compile")


class ServerModule(Case):
    FNS = """
typedef pmix_status_t (*pmix_a_fn_t)(void);
typedef pmix_status_t (*pmix_b_fn_t)(int x);
typedef pmix_status_t (*pmix_c_fn_t)(char *s);
"""

    def module(self, *members):
        return self.FNS + "typedef struct pmix_server_module_4_0_0_t {\n%s\n} pmix_server_module_t;\n" % \
            "\n".join("    %s;" % m for m in members)

    def test_appended(self):
        out = self.assertCompatible("", "",
                                    oldserver=self.module("pmix_a_fn_t a", "pmix_b_fn_t b"),
                                    newserver=self.module("pmix_a_fn_t a", "pmix_b_fn_t b",
                                                          "pmix_c_fn_t c"))
        self.assertIn("slot 2: pmix_c_fn_t c", out)

    def test_inserted(self):
        self.assertBreaks("", "", "INSERTED",
                          oldserver=self.module("pmix_a_fn_t a", "pmix_b_fn_t b"),
                          newserver=self.module("pmix_a_fn_t a", "pmix_c_fn_t c",
                                                "pmix_b_fn_t b"))

    def test_member_type_changed(self):
        self.assertBreaks("", "", "changed type",
                          oldserver=self.module("pmix_a_fn_t a", "pmix_b_fn_t b"),
                          newserver=self.module("pmix_a_fn_t a", "pmix_c_fn_t b"))


class Errors(Case):
    def test_unbalanced_header(self):
        o = self.tree("old", "typedef struct { int a; pmix_s_t;\n")
        n = self.tree("new", "")
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = check_compat.main(["--src", n, "--tgt", o])
        self.assertEqual(rc, 2)

    def test_report_only(self):
        o = self.tree("old", "#define PMIX_A 1\n")
        n = self.tree("new", "#define PMIX_A 2\n")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = check_compat.main(["--src", n, "--tgt", o, "--report-only"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
