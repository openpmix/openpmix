#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A constructor that forgets a member leaves it holding whatever was there.
#
# PMIX_NEW allocates and then runs the class's constructors; PMIX_CONSTRUCT
# does the same over storage the caller supplies.  Both now zero the object
# first, so a forgotten member is at least *deterministically* zero rather
# than whatever the heap or the stack last held - but zero is only the right
# answer when it happens to be.  Several members here deliberately start at
# something else: a count that must begin at SIZE_MAX so an unsent operation
# can never look complete, a generation that must begin at UINT32_MAX because
# zero is a real round.  For those, a forgotten member is still a bug, and now
# a quiet and repeatable one.
#
# Nothing downstream catches it.  The code compiles, the object looks
# plausible, and the mistake surfaces later as a value that is wrong in a way
# that has nothing to do with where it went wrong.  So look for it here: for
# every class, every member declared in its own struct body should be
# mentioned by its constructor.
#
# The test is deliberately "mentioned", not "assigned".  A member can be
# initialized through a macro (PMIX_CONSTRUCT(&p->list, ...)), through a
# helper that takes its address, or in a loop over an array of them, and
# recognizing each of those forms would make this a C parser.  What it is
# looking for is the member nobody thought about at all, which is the mistake
# that actually happens.

import os
import re
import sys

# Members that no constructor is expected to touch.
EXEMPT_NAMES = {
    # the class header, set up by the object system rather than by us
    'super',
    # A caddy's libevent member. The caddy pattern arms it at the moment the
    # caddy is posted (PRTE_PMIX_THREADSHIFT, prte_event_set), not when the
    # caddy is built, and libevent wants to do that arming itself - a
    # constructor that touched it would be writing state the event library
    # owns. Zeroed like everything else, which is an unarmed event.
    'ev',
    'tev',
}

# types whose innards are not ours to enumerate
EXEMPT_TYPES = {
    'pmix_object_t', 'pmix_list_item_t', 'pmix_list_t', 'pmix_event_t',
    'prte_event_t', 'pmix_data_buffer_t', 'pmix_bitmap_t', 'pmix_proc_t',
    'pmix_pointer_array_t', 'pmix_value_t', 'pmix_info_t', 'timeval',
}

CLASS_INSTANCE = re.compile(
    r'PMIX_CLASS_INSTANCE\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,'
    r'\s*([A-Za-z_]\w*|NULL)\s*,\s*([A-Za-z_]\w*|NULL)\s*\)')

# a member declaration: ... name;  or  ... name[...];  or  ... (*name)(...);
# group 1 the type words, group 2 any pointer stars, group 3 the member name
MEMBER = re.compile(r'^\s*((?:[A-Za-z_]\w*\s+)+)'
                    r'(\**)\s*'
                    r'(?:\(\s*\*\s*)?([A-Za-z_]\w*)\s*\)?'
                    r'(?:\s*\[[^\]]*\])*\s*(?:\([^)]*\))?\s*;\s*$')


def read(path):
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def sources(root):
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs
                   if d not in ('.git', 'autom4te.cache', 'oac')
                   and not d.startswith('vpath')
                   and not d.startswith('build')]
        for f in files:
            if f.endswith(('.c', '.h')):
                yield os.path.join(base, f)


TYPEDEF_OPEN = re.compile(r'typedef\s+struct\s*(?:[A-Za-z_]\w*\s*)?\{')


def struct_body(text, typename):
    """The body of `typedef struct {...} typename;` if it is written that way."""
    m = TYPEDEF_OPEN.search(text)
    while m:
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        tail = text[i:i + 200]
        if re.match(r'\}\s*' + re.escape(typename) + r'\s*;', tail):
            return text[m.end():i]
        m = TYPEDEF_OPEN.search(text, m.end())
    return None


def members_of(body):
    """(type, name) for members declared directly in this body."""
    out, depth = [], 0
    for line in body.split('\n'):
        opens, closes = line.count('{'), line.count('}')
        if depth == 0:
            m = MEMBER.match(line)
            if m and m.group(3) not in ('struct', 'union', 'enum'):
                words = m.group(1).split()
                typename = words[-1] if words else ''
                # A pointer member is a pointer whatever it points at, and
                # setting it to NULL is a complete answer - do not look inside
                # the type it names.
                if m.group(2):
                    typename = ''
                out.append((typename, m.group(3)))
        depth += opens - closes
    return out


def function_body(text, name):
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^)]*\)\s*\{', text)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[m.end():i]
        i += 1
    return None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    root = os.path.abspath(root)

    files = {p: strip_comments(read(p)) for p in sources(root)}
    problems = []

    for path, text in files.items():
        if not path.endswith('.c'):
            continue
        for typename, parent, ctor, _dtor in CLASS_INSTANCE.findall(text):
            if ctor == 'NULL':
                continue          # nothing to check
            body = None
            for other in files.values():
                body = struct_body(other, typename)
                if body is not None:
                    break
            if body is None:
                continue          # not a plain typedef struct - skip quietly
            ctor_body = function_body(text, ctor)
            if ctor_body is None:
                continue
            # A memset of the WHOLE object accounts for every member at once.
            # One of a single member does not, and must not exempt the rest -
            # constructors that clear an embedded header and then set fields
            # individually are common, and skipping them would hide every
            # omission in the constructor. So require the target to be the
            # object itself: a first argument with no -> or . in it.
            if re.search(r'\bmemset\s*\(\s*(?:\([^)]*\)\s*)?[A-Za-z_]\w*\s*,',
                         ctor_body):
                continue
            for mtype, name in members_of(body):
                if name in EXEMPT_NAMES:
                    continue
                if not re.search(r'\b' + re.escape(name) + r'\b', ctor_body):
                    problems.append((os.path.relpath(path, root), typename,
                                     ctor, name))
                    continue
                # The member is mentioned - but if it is itself a plain struct,
                # mentioning it is not the same as filling it in. This is the
                # shape the check was written for: an embedded signature whose
                # own members are set one at a time, and one of them forgotten.
                if not mtype or mtype in EXEMPT_TYPES:
                    continue
                sub_body = None
                for other in files.values():
                    sub_body = struct_body(other, mtype)
                    if sub_body is not None:
                        break
                if sub_body is None:
                    continue
                # handed to something wholesale (a constructor, a memset, a
                # loader taking its address)? then it is accounted for.
                if re.search(r'&\s*\w+\s*->\s*' + re.escape(name) + r'\b',
                             ctor_body):
                    continue
                for _st, sub in members_of(sub_body):
                    if sub in EXEMPT_NAMES:
                        continue
                    # allow an index in between: an array of these is
                    # filled in a loop, as p->tree[t].member = ...
                    if re.search(re.escape(name) + r'\s*(?:\[[^\]]*\])?\s*'
                                 r'(?:\.|->)\s*' + re.escape(sub) + r'\b',
                                 ctor_body):
                        continue
                    problems.append((os.path.relpath(path, root), typename,
                                     ctor, '%s.%s' % (name, sub)))

    here = os.path.dirname(os.path.abspath(__file__))
    baseline_path = os.path.join(here, 'ctor_coverage_baseline.txt')
    baseline = set()
    for line in read(baseline_path).split('\n'):
        line = line.split('#', 1)[0].strip()
        if line:
            baseline.add(line)

    found = set('%s %s.%s' % (p, t, n) for p, t, _c, n in problems)
    new_problems = sorted(found - baseline)
    fixed = sorted(baseline - found)

    rc = 0
    if new_problems:
        print('Constructors that never mention a member of their own class:')
        print()
        for key in new_problems:
            path, member = key.split(' ', 1)
            print('  %s: %s is never set by its constructor' % (path, member))
        print()
        print('A member nobody sets holds whatever the object was zeroed to,')
        print('which is right only where zero is the intended default. Several')
        print('here deliberately start elsewhere - a count that begins at')
        print('SIZE_MAX, a generation that begins at UINT32_MAX - and for those')
        print('a forgotten member is a bug that now reproduces quietly rather')
        print('than sometimes.')
        print()
        print('Set it explicitly. If zero really is the default, assign it')
        print('anyway, so the next reader can see it was a decision.')
        rc = 1

    if fixed:
        if rc:
            print()
        print('These were listed in %s but no longer occur:' %
              os.path.basename(baseline_path))
        print()
        for key in fixed:
            print('  %s' % key)
        print()
        print('Somebody fixed them - delete those lines, so the baseline stays')
        print('a record of what is left rather than of what once was.')
        rc = 1

    if 0 == rc:
        print('all constructors account for their own members '
              '(%d known omissions still listed)' % len(baseline))
    return rc


if __name__ == '__main__':
    sys.exit(main())
