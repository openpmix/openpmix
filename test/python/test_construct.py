#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Unit tests for bindings/python/construct.py, the generator that harvests
# the C headers into pmix_constants.pxi and pmix_constants.pxd.
#
# The generator is plain Python and needs neither a built extension nor a
# library, so these run anywhere.  They matter because its output is what
# every Python constant in the bindings *is*: a header the generator
# mis-parses does not fail the build, it silently produces a constant with
# the wrong value, and the first symptom is an attribute comparison that
# never matches at run time.
#
# Run directly with:
#     ./test_construct.py

import os
import sys
import tempfile
import unittest


def _load_construct():
    """Import construct.py from the source tree beside us."""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    path = os.path.join(root, "bindings", "python")
    if path not in sys.path:
        sys.path.insert(0, path)
    import construct
    return construct


try:
    construct = _load_construct()
except ImportError:                                  # pragma: no cover
    print("construct.py not importable - skipping", file=sys.stderr)
    sys.exit(77)


class _Options(object):
    """The subset of construct.py's option object harvest_constants reads."""

    def __init__(self, directory):
        self.src = directory
        self.includedir = directory


class _Harvest(unittest.TestCase):
    """Run the generator over a synthetic header and capture its output."""

    def harvest(self, text, name="pmix_common.h"):
        import io
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
            constants = io.StringIO()
            definitions = io.StringIO()
            construct.takeconst = True
            construct.takeapis = True
            construct.takedtypes = True
            rc = construct.harvest_constants(_Options(d), name,
                                             constants, definitions)
        return rc, constants.getvalue(), definitions.getvalue()

    @staticmethod
    def _assignments(constants):
        """The 'NAME = value' lines, as a {name: value} mapping.

        The generator pads the name out to a column, so parse on the '='
        rather than on whitespace.
        """
        out = {}
        for line in constants.splitlines():
            if line.startswith("#") or "=" not in line:
                continue
            name, _eq, value = line.partition("=")
            out[name.strip()] = value.strip()
        return out

    def assertConstant(self, constants, name, value):
        got = self._assignments(constants)
        self.assertIn(name, got,
                      "%s was not generated:\n%s" % (name, constants))
        self.assertEqual(got[name], value,
                         "%s came out as %s" % (name, got[name]))

    def assertNoConstant(self, constants, name):
        self.assertNotIn(name, self._assignments(constants),
                         "%s should not have been generated:\n%s"
                         % (name, constants))


class TestEnums(_Harvest):
    """typedef enum bodies.

    The generator assigns each enumerator the value the C compiler would.
    Getting this wrong is invisible: the constant still exists, it just
    does not mean what the C side means by it, and these values cross the
    server-module callback boundary.
    """

    def test_sequential_values(self):
        rc, constants, _defs = self.harvest("""
typedef enum {
    PMIX_A,
    PMIX_B,
    PMIX_C
} pmix_letter_t;
""")
        self.assertEqual(rc, 0)
        self.assertConstant(constants, "PMIX_A", "0")
        self.assertConstant(constants, "PMIX_B", "1")
        self.assertConstant(constants, "PMIX_C", "2")

    def test_comment_lines_are_not_enumerators(self):
        # a comment inside the body used to become a constant of its own,
        # and shifted every enumerator after it by one.  pmix_common.h
        # carries a warning asking contributors not to write one; the
        # generator should simply cope.
        rc, constants, _defs = self.harvest("""
typedef enum {
    PMIX_A,
    /* what B is for */
    PMIX_B,

    // and C
    PMIX_C
} pmix_letter_t;
""")
        self.assertEqual(rc, 0)
        self.assertConstant(constants, "PMIX_A", "0")
        self.assertConstant(constants, "PMIX_B", "1")
        self.assertConstant(constants, "PMIX_C", "2")

    def test_explicit_values_are_honored(self):
        # an ignored "= value" renumbers the whole enum from that point on
        rc, constants, _defs = self.harvest("""
typedef enum {
    PMIX_A = 4,
    PMIX_B,
    PMIX_C = 0x10,
    PMIX_D
} pmix_letter_t;
""")
        self.assertEqual(rc, 0)
        self.assertConstant(constants, "PMIX_A", "4")
        self.assertConstant(constants, "PMIX_B", "5")
        self.assertConstant(constants, "PMIX_C", "16")
        self.assertConstant(constants, "PMIX_D", "17")

    def test_enum_before_any_define(self):
        # the enumerator name and value used to be stored through "tokens",
        # a list left over from the last #define seen in the file, so an
        # enum that came first raised NameError
        rc, constants, _defs = self.harvest("""
typedef enum {
    PMIX_ONLY
} pmix_only_t;
#define PMIX_LATER   7
""")
        self.assertEqual(rc, 0)
        self.assertConstant(constants, "PMIX_ONLY", "0")
        self.assertConstant(constants, "PMIX_LATER", "7")

    def test_enum_type_is_declared(self):
        _rc, _constants, defs = self.harvest("""
typedef enum {
    PMIX_A
} pmix_letter_t;
""")
        self.assertIn("typedef int pmix_letter_t", defs)

    def test_unparsable_value_is_reported(self):
        # better to stop the build than to emit a constant that is wrong
        rc, _constants, _defs = self.harvest("""
typedef enum {
    PMIX_A = SOME_OTHER_MACRO
} pmix_letter_t;
""")
        self.assertNotEqual(rc, 0)

    def test_unterminated_enum_is_reported(self):
        rc, _constants, _defs = self.harvest("""
typedef enum {
    PMIX_A,
    PMIX_B
""")
        self.assertNotEqual(rc, 0)


class TestDefines(_Harvest):
    """#define constants, in their three flavors."""

    def test_string_constant(self):
        _rc, constants, defs = self.harvest("""
#define PMIX_SOMETHING   "pmix.something"
""")
        # the Python-visible constant decodes, so it compares equal to the
        # keys the library hands back
        self.assertIn("PMIX_SOMETHING", constants)
        self.assertIn(".decode('ascii')", constants)
        self.assertIn("cdef const char* _PMIX_SOMETHING", defs)

    def test_numeric_constant(self):
        _rc, constants, _defs = self.harvest("""
#define PMIX_COUNT   42
""")
        self.assertConstant(constants, "PMIX_COUNT", "42")

    def test_hex_constant(self):
        _rc, constants, _defs = self.harvest("""
#define PMIX_FLAG   0x0004
""")
        self.assertConstant(constants, "PMIX_FLAG", "0x0004")

    def test_uint32_max_is_converted(self):
        # Python does not know UINT32_MAX, so the generator substitutes it
        _rc, constants, _defs = self.harvest("""
#define PMIX_RANK_UNDEF     UINT32_MAX
#define PMIX_RANK_WILDCARD  UINT32_MAX-1
""")
        self.assertConstant(constants, "PMIX_RANK_UNDEF", "0xffffffff")
        self.assertConstant(constants, "PMIX_RANK_WILDCARD", "0xfffffffe")

    def test_error_constant(self):
        _rc, constants, defs = self.harvest("""
#define PMIX_ERR_BAD   -27
""")
        self.assertIn("PMIX_ERR_BAD", constants)
        self.assertIn("cdef int _PMIX_ERR_BAD", defs)

    def test_function_like_macros_are_skipped(self):
        # these are not constants, and there is nothing to bind
        _rc, constants, _defs = self.harvest("""
#define PMIx_Something(a, b)   do_it(a, b)
#define PMIX_HAVE_VISIBILITY 1
""")
        self.assertNoConstant(constants, "PMIx_Something(a,")
        self.assertNoConstant(constants, "PMIX_HAVE_VISIBILITY")


class TestApis(_Harvest):
    """PMIX_EXPORT declarations."""

    def test_single_line_api(self):
        _rc, _constants, defs = self.harvest("""
PMIX_EXPORT pmix_status_t PMIx_Do_thing(int a);
""")
        self.assertIn("pmix_status_t PMIx_Do_thing(int a) nogil", defs)

    def test_void_argument_is_snipped(self):
        # Cython does not accept "void" as an argument list
        _rc, _constants, defs = self.harvest("""
PMIX_EXPORT int PMIx_Initialized(void);
""")
        self.assertIn("PMIx_Initialized()", defs)
        self.assertNotIn("PMIx_Initialized(void)", defs)

    def test_bool_becomes_bint(self):
        # Cython has no "bool"; leaving one in the .pxd fails the build
        _rc, _constants, defs = self.harvest("""
PMIX_EXPORT bool PMIx_Is_thing(bool flag);
""")
        self.assertIn("bint", defs)
        self.assertNotIn("bool", defs)

    def test_multi_line_api_is_collected(self):
        _rc, _constants, defs = self.harvest("""
PMIX_EXPORT pmix_status_t PMIx_Do_thing(int a,
                                        bool b,
                                        char *c);
""")
        self.assertIn("PMIx_Do_thing", defs)
        self.assertIn("bint b,", defs)
        self.assertIn("char *c) nogil", defs)

    def test_lines_after_a_multi_line_api_are_not_rescanned(self):
        # the harvest loop used to be a "for n in range(len(lines))" whose
        # body advanced n by hand, so every line a multi-line declaration
        # swallowed was read a second time
        _rc, constants, defs = self.harvest("""
PMIX_EXPORT pmix_status_t PMIx_Do_thing(int a,
                                        int b);
#define PMIX_AFTER   9
""")
        self.assertConstant(constants, "PMIX_AFTER", "9")
        self.assertEqual(defs.count("PMIx_Do_thing"), 1)


class TestTypedefs(_Harvest):
    """typedef handling, including the struct bodies."""

    def test_simple_typedef(self):
        _rc, _constants, defs = self.harvest("""
typedef uint16_t pmix_thing_t;
""")
        self.assertIn("ctypedef uint16_t pmix_thing_t", defs)

    def test_bool_typedef_becomes_bint(self):
        _rc, _constants, defs = self.harvest("""
typedef bool pmix_flag_t;
""")
        self.assertIn("bint", defs)
        self.assertNotIn("bool", defs)

    def test_predeclaration_is_skipped(self):
        # "typedef struct foo foo" declares nothing Cython needs
        _rc, _constants, defs = self.harvest("""
typedef struct pmix_thing pmix_thing;
""")
        self.assertNotIn("pmix_thing pmix_thing", defs)

    def test_struct_body(self):
        _rc, _constants, defs = self.harvest("""
typedef struct {
    char name[PMIX_MAX_NSLEN+1];
    int rank;
} pmix_thing_t;
""")
        self.assertIn("ctypedef struct pmix_thing_t:", defs)
        # the dimension has to become a literal - Cython cannot see the
        # C macro
        self.assertIn("char name[256]", defs)
        self.assertIn("int rank", defs)

    def test_missing_file_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            import io
            rc = construct.harvest_constants(_Options(d), "no_such.h",
                                             io.StringIO(), io.StringIO())
        self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
