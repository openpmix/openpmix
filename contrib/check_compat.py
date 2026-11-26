#!/usr/bin/env python3
#
# Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Compare the public interface of two PMIx source trees.
#
# PMIx keeps its public interface forever: nothing public is removed,
# and nothing a compiled program depends on - a constant's value, an
# attribute's string, a function's parameter types, a structure's
# layout - is changed. This script harvests the public headers of a
# newer tree (--src) and an older one (--tgt), and reports what the
# newer one added, changed, and removed. Removals and changes that a
# program built against the older tree would notice are BREAKING, and
# make the script exit 1.
#
# pmix_server_module_t is the one structure that may grow: hosts
# supply it and the library copies it, so new members may be appended,
# but each existing member must keep its slot. See the ABI STABILITY
# section of pmix_server_module_t(5).
#
# Structure layouts are compared from their declarations unless
# --compile is given, in which case a probe is compiled against each
# tree and the sizes and offsets the compiler reports are compared.
# That is exact: a union that gains members is compatible as long as
# none of them is larger than what it already held.
#
# Usage:
#   check_compat.py --src <newer>/include --tgt <older>/include [--compile]
#
# Exit status: 0 if nothing breaks, 1 if something does (unless
# --report-only), 2 on a usage, parse, or compile error.

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HEADERS = ["pmix_common.h.in", "pmix.h", "pmix_server.h",
           "pmix_tool.h", "pmix_deprecated.h"]

SERVER_MODULE = "pmix_server_module_t"

# words that are part of a parameter's type, never its name
TYPE_WORDS = {"void", "char", "short", "int", "long", "float", "double",
              "signed", "unsigned", "bool", "const", "volatile",
              "struct", "union", "enum", "restrict"}

# names a #define may carry without being part of the interface
SKIP_DEFINES = {"PMIX_EXPORT"}

# a name may change how it is provided without a program noticing
MOVES = {("Macros", "APIs"), ("APIs", "Macros"),
         ("Constants", "Enumerators"), ("Enumerators", "Constants")}


class ParseError(Exception):
    pass


# ---------------------------------------------------------------- lexing

_COMMENT_OR_LITERAL = re.compile(
    r'"(?:\\.|[^"\\\n])*"'      # string literal
    r"|'(?:\\.|[^'\\\n])*'"     # character literal
    r'|/\*.*?\*/'               # block comment
    r'|//[^\n]*',               # line comment
    re.S)

_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\w+|\.\.\.|->|<<|>>|\S')


def strip_comments(text):
    """Remove comments, keeping string literals and line numbers."""
    def repl(m):
        s = m.group(0)
        if s.startswith('/*'):
            return ' ' + '\n' * s.count('\n')
        if s.startswith('//'):
            return ''
        return s
    return _COMMENT_OR_LITERAL.sub(repl, text)


def tokens(text):
    return _TOKEN.findall(text)


def norm(text):
    """Whitespace-insensitive form of a piece of C."""
    return ' '.join(tokens(text))


def skip_literal(text, i):
    q = text[i]
    i += 1
    while i < len(text) and text[i] != q:
        i += 2 if text[i] == '\\' else 1
    return i + 1


def matching(text, start, open_ch, close_ch):
    """Index of the bracket closing the one at text[start]."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == '"' or c == "'":
            i = skip_literal(text, i)
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ParseError("unbalanced '%s' in: %s" % (open_ch, ' '.join(text.split())[:120]))


def split_top(text, sep):
    """Split text at each sep not nested in brackets."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"' or c == "'":
            j = skip_literal(text, i)
            cur.append(text[i:j])
            i = j
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == sep and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    parts.append(''.join(cur))
    return [p.strip() for p in parts if p.strip()]


def statements(code):
    """Split code into top-level statements."""
    out, start, depth, i = [], 0, 0, 0
    while i < len(code):
        c = code[i]
        if c == '"' or c == "'":
            i = skip_literal(code, i)
            continue
        if c in '({[':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                # the brace closing extern "C" - not part of a statement
                start = i + 1
            else:
                depth -= 1
                if depth == 0 and c == '}':
                    # the body of an inline function ends its statement
                    head = code[start:i].lstrip()
                    if head.startswith("static") and re.search(r'\)\s*\{', head):
                        out.append(code[start:i + 1].strip())
                        start = i + 1
        elif c == ';' and depth == 0:
            out.append(code[start:i].strip())
            start = i + 1
        i += 1
    if depth != 0:
        raise ParseError("unbalanced brackets at end of file")
    return [s for s in out if s]


def declarator_name(text):
    """The name a declaration declares - its last identifier, ignoring
    array bounds, bit-field widths, and attributes."""
    text = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', text)
    text = re.sub(r'\[[^\]]*\]', ' ', text)
    text = re.sub(r':\s*\d+\s*$', ' ', text)
    ids = re.findall(r'[A-Za-z_]\w*', text)
    return ids[-1] if ids else None


def int_value(text):
    """The value of an integer constant expression such as '0x01',
    '(pmix_status_t) -12', or 'UINT8_C(3)', or None."""
    t = norm(text)
    for _ in range(4):
        # strip outer parentheses and leading casts
        m = re.fullmatch(r'\( (.*) \)', t)
        if m and matching(t, 0, '(', ')') == len(t) - 1:
            t = m.group(1)
            continue
        m = re.fullmatch(r'\( (?:const )?[A-Za-z_]\w*(?: [A-Za-z_]\w*)* \) (.*)', t)
        if m:
            t = m.group(1)
            continue
        break
    m = re.fullmatch(r'(-|\+)? ?(0[xX][0-9a-fA-F]+|\d+)[uUlL]*', t)
    if not m:
        return None
    v = int(m.group(2), 0) if not re.fullmatch(r'0\d+', m.group(2)) else int(m.group(2), 8)
    return -v if m.group(1) == '-' else v


def param_type(p):
    """A parameter's type, without its name: what the ABI depends on."""
    t = tokens(p)
    if t in (['void'], ['...']):
        return ' '.join(t)
    # function pointer parameter: ret (*name)(args)
    for i in range(len(t) - 3):
        if t[i] == '(' and t[i + 1] == '*' and re.match(r'\w+$', t[i + 2]) and t[i + 3] == ')':
            return ' '.join(t[:i + 2] + t[i + 3:])
    # an array parameter is a pointer
    out, i = [], 0
    while i < len(t):
        if t[i] == '[':
            while i < len(t) and t[i] != ']':
                i += 1
            out.append('*')
        else:
            out.append(t[i])
        i += 1
    # drop a trailing name
    names = [j for j, x in enumerate(out) if re.match(r'[A-Za-z_]\w*$', x)]
    if len(names) > 1:
        j = names[-1]
        last = max(k for k, x in enumerate(out) if x != '*')
        if j == last and out[j] not in TYPE_WORDS and not out[j].endswith("_t"):
            del out[j]
    return ' '.join(out)


def signature(ret, params):
    return (norm(ret),) + tuple(param_type(p) for p in split_top(params, ','))


def evaluate(expr, known):
    """The value of an enumerator's initializer: an int if it can be
    computed, else the expression as written."""
    e = norm(expr)
    v = int_value(e)
    if v is not None:
        return v
    src = re.sub(r'\b([A-Za-z_]\w*)\b',
                 lambda m: str(known[m.group(1)]) if isinstance(known.get(m.group(1)), int)
                 else m.group(1), e)
    if re.fullmatch(r'[0-9xXa-fA-F\s()+\-*/|&<>~]*', src):
        try:
            return int(eval(src, {"__builtins__": {}}))
        except Exception:
            pass
    return e


# ------------------------------------------------------------ harvesting

class Item:
    """One element of the public interface."""
    def __init__(self, category, name, key, text, header, **extra):
        self.category = category    # e.g. "Attributes", "APIs"
        self.name = name
        self.key = key              # what must match for compatibility
        self.text = text            # how to show it
        self.header = header
        self.extra = extra


class Interface:
    def __init__(self, root):
        self.root = root
        self.items = {}             # (category, name) -> [Item, ...]
        self.missing = []
        self.layout = None          # filled in by --compile

    def add(self, item):
        lst = self.items.setdefault((item.category, item.name), [])
        if all(i.key != item.key for i in lst):
            lst.append(item)

    def get(self, cat, name):
        return self.items.get((cat, name))

    def harvest(self):
        for h in HEADERS:
            path = os.path.join(self.root, h)
            if not os.path.exists(path):
                self.missing.append(h)
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            try:
                self.harvest_text(text, h)
            except ParseError as e:
                raise ParseError("%s: %s" % (path, e))

    def counts(self):
        c = {}
        for cat, _ in self.items:
            c[cat] = c.get(cat, 0) + 1
        return ", ".join("%d %s" % (n, k) for k, n in sorted(c.items()))

    def harvest_text(self, text, header):
        text = strip_comments(text).replace('\\\n', ' ')
        code = []
        for line in text.split('\n'):
            s = line.strip()
            if s.startswith('#'):
                self.directive(s, header)
                code.append('')
            else:
                code.append(line)
        code = re.sub(r'extern\s+"C"\s*\{', ' ', '\n'.join(code))
        for stmt in statements(code):
            self.statement(stmt, header)

    # -- preprocessor

    def directive(self, line, header):
        m = re.match(r'#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)$', line)
        if not m:
            return
        name, params, value = m.group(1), m.group(2), m.group(3).strip()
        if name in SKIP_DEFINES or name.startswith("PMIX_HAVE_VISIB"):
            return
        if params is not None:
            plist = [p.strip() for p in params[1:-1].split(',') if p.strip()]
            # only how many arguments a macro takes matters to its users
            self.add(Item("Macros", name, tuple('...' if x == '...' else 'arg' for x in plist),
                          "%s(%s)" % (name, ", ".join(plist)), header, body=norm(value)))
        elif not value:
            # include guards and other flags with no value
            return
        elif value.startswith('"'):
            self.add(Item("Attributes", name, value, "%s %s" % (name, value), header))
        elif value.startswith('{'):
            self.add(Item("Initializers", name, norm(value), name, header))
        else:
            v = int_value(value)
            self.add(Item("Constants", name, v if v is not None else norm(value),
                          "%s %s" % (name, ' '.join(value.split())), header))

    # -- declarations

    def statement(self, s, header):
        flat = ' '.join(s.split())
        if flat.startswith("typedef "):
            self.typedef(s, flat, header)
        elif flat.startswith("PMIX_EXPORT "):
            self.export(flat[len("PMIX_EXPORT "):], header)
        elif flat.startswith("static "):
            self.inline(flat, header)
        elif re.match(r'(struct|union|enum)\s+\w+\s*\{', flat):
            self.aggregate(s, flat, None, header)

    def export(self, decl, header):
        decl = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', decl).strip()
        m = re.search(r'(\w+)\s*\(', decl)
        if m is None:
            name = declarator_name(decl)
            self.add(Item("Variables", name, norm(decl), decl, header))
            return
        name, ret = m.group(1), decl[:m.start()].strip()
        params = decl[m.end():matching(decl, m.end() - 1, '(', ')')]
        self.add(Item("APIs", name, signature(ret, params),
                      "%s %s(%s)" % (ret, name, ' '.join(params.split())), header))

    def inline(self, flat, header):
        head = flat.split('{', 1)[0]
        m = re.search(r'(\w+)\s*\(', head)
        if m is None:
            return
        ret = re.sub(r'^static\s+(inline\s+)?', '', head[:m.start()]).strip()
        params = head[m.end():matching(head, m.end() - 1, '(', ')')]
        self.add(Item("Inline functions", m.group(1), signature(ret, params),
                      "%s %s(%s)" % (ret, m.group(1), params), header))

    def typedef(self, s, flat, header):
        if '{' in flat:
            close = matching(s, s.index('{'), '{', '}')
            name = declarator_name(s[close + 1:])
            if name:
                self.aggregate(s, flat, name, header)
            return
        m = re.match(r'typedef\s+(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$', flat, re.S)
        if m:
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
            self.add(Item("Function types", name, signature(ret, params),
                          "%s (*%s)(%s)" % (ret, name, params), header))
            return
        name = declarator_name(flat)
        if name:
            self.add(Item("Types", name, norm(flat), flat, header))

    def aggregate(self, s, flat, name, header):
        m = re.match(r'(?:typedef\s+)?(struct|union|enum)\s*(\w*)\s*\{', flat)
        kind, tag = m.group(1), m.group(2)
        if name is None:
            name = "%s %s" % (kind, tag)
        start = s.index('{')
        body = s[start + 1:matching(s, start, '{', '}')]
        if kind == "enum":
            self.enum(name, body, header)
            return
        members = [norm(x) for x in split_top(body, ';')]
        self.add(Item("Structures", name, tuple(members), name, header,
                      members=members, kind=kind))

    def enum(self, name, body, header):
        nxt, known = 0, {}
        for e in split_top(body, ','):
            if '=' in e:
                ename, expr = [x.strip() for x in e.split('=', 1)]
                val = evaluate(expr, known)
            else:
                ename, val = e.strip(), nxt
            known[ename] = val
            nxt = val + 1 if isinstance(val, int) else "%s + 1" % val
            self.add(Item("Enumerators", ename, val, "%s = %s" % (ename, val), header))
        self.add(Item("Types", name, "enum", "enum %s" % name, header))


# --------------------------------------------------------- layout probe

def member_names(decl):
    """The names one member declaration declares, e.g. 'char *a, *b'."""
    if re.search(r':\s*\d+\s*$', decl):
        # a bit-field has no offset
        return []
    if '{' in decl:
        # a nested aggregate: 'union { ... } data', or anonymous
        name = declarator_name(decl[matching(decl, decl.index('{'), '{', '}') + 1:])
        return [name] if name else []
    return [declarator_name(d) for d in split_top(decl, ',')]


def stage(root, where):
    """Copy a tree's public headers where a compiler can find them."""
    os.makedirs(where)
    for h in HEADERS:
        src = os.path.join(root, h)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(where, h[:-3] if h.endswith(".in") else h))
    # pmix_version.h is generated; its values do not affect any layout
    text = ""
    for ver in ("pmix_version.h.in", "pmix_version.h"):
        if os.path.exists(os.path.join(root, ver)):
            with open(os.path.join(root, ver)) as f:
                text = re.sub(r'@\w+@', '0', f.read())
            break
    with open(os.path.join(where, "pmix_version.h"), "w") as f:
        f.write(text)


def probe_layout(tree, structs, workdir, cc):
    """Compile and run a program reporting the size of each structure
    and the offset and size of each named member."""
    inc = os.path.join(workdir, "include")
    stage(tree.root, inc)
    lines = ['#include <stddef.h>', '#include <stdio.h>']
    for h in ("pmix.h", "pmix_server.h", "pmix_tool.h"):
        if os.path.exists(os.path.join(inc, h)):
            lines.append('#include <%s>' % h)
    lines.append('int main(void) {')
    for s in structs:
        item = tree.get("Structures", s)[0]
        lines.append('    printf("%s|%%zu\\n", sizeof(%s));' % (s, s))
        for decl in item.extra["members"]:
            for m in member_names(decl):
                if m:
                    lines.append('    printf("%s.%s|%%zu|%%zu\\n", offsetof(%s, %s), '
                                 'sizeof(((%s *)0)->%s));' % (s, m, s, m, s, m))
    lines += ['    return 0;', '}']
    src = os.path.join(workdir, "probe.c")
    exe = os.path.join(workdir, "probe")
    with open(src, "w") as f:
        f.write('\n'.join(lines) + '\n')
    r = subprocess.run(cc + ["-w", "-I", inc, src, "-o", exe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise ParseError("could not compile the layout probe for %s:\n%s" %
                         (tree.root, r.stderr[-2000:]))
    r = subprocess.run([exe], capture_output=True, text=True, check=True)
    layout = {}
    for line in r.stdout.split('\n'):
        if line:
            f = line.split('|')
            layout[f[0]] = tuple(int(x) for x in f[1:])
    return layout


# ------------------------------------------------------------ comparison

class Report:
    def __init__(self):
        self.added = []         # (category, text)
        self.compatible = []
        self.breaking = []

    @staticmethod
    def section(title, entries):
        print(title)
        if not entries:
            print("\t(none)\n")
            return
        cat = None
        for c, text in sorted(entries):
            if c != cat:
                print("\n  %s" % c.upper())
                cat = c
            for line in text.split('\n'):
                print("\t" + line)
        print("")


def describe(items):
    return " | ".join(sorted({i.text for i in items}))


def strip_const(sig):
    return tuple(' '.join(x for x in p.split() if x != 'const') for p in sig)


def compatible_sig(new, old):
    """A new signature that only adds const to what a parameter points at
    accepts everything the old one did."""
    if len(new) != len(old) or strip_const(new) != strip_const(old):
        return False
    return all(n.split().count('const') >= o.split().count('const')
               for n, o in zip(new[1:], old[1:])) and new[0] == old[0]


def compare(new, old, report, structs_compiled):
    keys = set(new.items) | set(old.items)
    moved = set()
    # a name that changed how it is provided - a macro that became a
    # function, a #define that became an enumerator
    for cat, name in keys:
        if (cat, name) in old.items and (cat, name) not in new.items:
            for a, b in MOVES:
                if a == cat and (b, name) in new.items and (b, name) not in old.items:
                    o, n = old.items[(a, name)], new.items[(b, name)]
                    if a in ("Constants", "Enumerators") and \
                            {i.key for i in o} != {i.key for i in n}:
                        continue
                    report.compatible.append((b, "%s: was in %s, now in %s" %
                                              (describe(n), a.lower(), b.lower())))
                    moved |= {(a, name), (b, name)}

    for key in sorted(keys, key=str):
        if key in moved:
            continue
        cat, name = key
        n, o = new.items.get(key), old.items.get(key)
        if o is None:
            report.added.append((cat, describe(n)))
            continue
        if n is None:
            report.breaking.append((cat, "REMOVED: " + describe(o)))
            continue
        if cat == "Structures":
            compare_struct(name, n[0], o[0], new, old, report, structs_compiled)
            continue
        nkeys = {i.key for i in n}
        okeys = {i.key for i in o}
        if nkeys == okeys:
            if cat == "Macros" and {i.extra["body"] for i in n} != {i.extra["body"] for i in o}:
                report.compatible.append((cat, "%s: definition changed" % describe(n)))
            continue
        if cat == "Initializers":
            report.compatible.append((cat, "%s: default values changed" % name))
            continue
        if cat in ("APIs", "Function types", "Inline functions") and \
                all(any(compatible_sig(nk, ok) for nk in nkeys) for ok in okeys):
            report.compatible.append((cat, "%s\n  was: %s" % (describe(n), describe(o))))
            continue
        if okeys <= nkeys:
            # a conditional definition gained an alternative
            report.compatible.append((cat, "%s: gained a definition" % describe(n)))
            continue
        report.breaking.append((cat, "CHANGED: %s\n  was: %s" % (describe(n), describe(o))))


def member_diff(old, new):
    lines = []
    for i in range(max(len(old), len(new))):
        a = old[i] if i < len(old) else None
        b = new[i] if i < len(new) else None
        if a == b:
            continue
        if a is None:
            lines.append("  member %d: added   %s" % (i, b))
        elif b is None:
            lines.append("  member %d: removed %s" % (i, a))
        else:
            lines.append("  member %d: was     %s\n            now     %s" % (i, a, b))
    return '\n'.join(lines)


def compare_struct(name, n, o, new, old, report, compiled):
    nm, om = n.extra["members"], o.extra["members"]
    if name == SERVER_MODULE:
        check_server_module(nm, om, report)
        return
    if nm == om:
        return
    if not compiled:
        report.breaking.append(("Structures", "%s changed\n%s\n  (run with --compile "
                                "to measure whether its layout did)" %
                                (name, member_diff(om, nm))))
        return
    nl, ol = new.layout, old.layout
    problems = []
    if nl[name] != ol[name]:
        problems.append("  size changed from %d to %d" % (ol[name][0], nl[name][0]))
    for decl in om:
        for m in member_names(decl):
            k = "%s.%s" % (name, m)
            if k not in nl:
                problems.append("  member %s was removed or renamed" % m)
            elif nl[k] != ol[k]:
                problems.append("  member %s moved from offset %d (size %d) to %d (size %d)" %
                                (m, ol[k][0], ol[k][1], nl[k][0], nl[k][1]))
    if problems:
        report.breaking.append(("Structures", "%s changed layout\n%s\n%s" %
                                (name, '\n'.join(problems), member_diff(om, nm))))
    else:
        report.compatible.append(("Structures", "%s: declaration changed, layout "
                                  "unchanged (%d bytes)\n%s" %
                                  (name, nl[name][0], member_diff(om, nm))))


def check_server_module(new, old, report):
    """The host's module is copied by value and its members are found by
    offset: each member of the older definition must keep its slot and
    type, and new members may only follow them."""
    newslot = {declarator_name(d): i for i, d in enumerate(new)}
    problems = []
    for i, d in enumerate(old):
        if i < len(new) and new[i] == d:
            continue
        nm = declarator_name(d)
        if nm not in newslot:
            problems.append("slot %d: %s was REMOVED" % (i, d))
        elif newslot[nm] != i:
            problems.append("slot %d: %s MOVED to slot %d" % (i, nm, newslot[nm]))
        else:
            problems.append("slot %d: %s changed type to %s" % (i, d, new[i]))
    oldnames = {declarator_name(d) for d in old}
    for i, d in enumerate(new):
        if declarator_name(d) not in oldnames:
            if i < len(old):
                problems.append("slot %d: %s was INSERTED ahead of existing members" % (i, d))
            else:
                report.added.append(("Server module members", "slot %d: %s" % (i, d)))
    if problems:
        report.breaking.append(("Server module layout", '\n'.join(problems)))


# ------------------------------------------------------------------ main

def main(argv=None):
    p = argparse.ArgumentParser(
        description="Compare the public interface of two PMIx trees")
    p.add_argument("--src", required=True,
                   help="directory holding the NEWER tree's public headers")
    p.add_argument("--tgt", required=True,
                   help="directory holding the OLDER tree's public headers")
    p.add_argument("--compile", action="store_true",
                   help="measure structure layouts with the compiler ($CC, or cc)")
    p.add_argument("--report-only", action="store_true",
                   help="exit 0 even if something breaks")
    p.add_argument("--all", action="store_true",
                   help="also list additions and compatible changes")
    p.add_argument("--debug", action="store_true",
                   help="report what was harvested from each tree")
    opts = p.parse_args(argv)

    trees = []
    for d in (opts.src, opts.tgt):
        if not os.path.isdir(d):
            print("Directory %s does not exist" % d, file=sys.stderr)
            return 2
        t = Interface(d)
        try:
            t.harvest()
        except ParseError as e:
            print("Could not parse the headers in %s: %s" % (d, e), file=sys.stderr)
            return 2
        if len(t.missing) == len(HEADERS):
            print("No PMIx headers found in %s" % d, file=sys.stderr)
            return 2
        if opts.debug:
            print("%s: %s" % (d, t.counts()), file=sys.stderr)
        trees.append(t)
    new, old = trees

    if opts.compile:
        # only structures both trees define, and only those that changed
        changed = [n for (c, n) in old.items
                   if c == "Structures" and n != SERVER_MODULE and (c, n) in new.items
                   and new.get(c, n)[0].extra["members"] != old.get(c, n)[0].extra["members"]]
        cc = os.environ.get("CC", "cc").split()
        work = tempfile.mkdtemp(prefix="pmix-compat-")
        try:
            for i, t in enumerate(trees):
                t.layout = probe_layout(t, changed, os.path.join(work, str(i)), cc)
        except (ParseError, OSError, subprocess.CalledProcessError) as e:
            print(e, file=sys.stderr)
            return 2
        finally:
            shutil.rmtree(work, ignore_errors=True)

    report = Report()
    compare(new, old, report, opts.compile)

    print("Comparing %s (newer) against %s (older)\n" % (opts.src, opts.tgt))
    for h in sorted(set(old.missing) - set(new.missing)):
        print("NOTE: %s has no %s\n" % (opts.tgt, h))
    for h in sorted(set(new.missing) - set(old.missing)):
        print("NOTE: %s has no %s\n" % (opts.src, h))
    Report.section("BREAKING CHANGES - a program built against the older "
                   "headers would notice these", report.breaking)
    if opts.all:
        Report.section("COMPATIBLE CHANGES", report.compatible)
        Report.section("ADDITIONS", report.added)
    print("Summary: %d breaking, %d compatible changes, %d additions" %
          (len(report.breaking), len(report.compatible), len(report.added)))

    if report.breaking and not opts.report_only:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
