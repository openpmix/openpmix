#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# src/util/convert-help.py reports an "#include" line that appears in a
# help file before any [topic] header, skips the line, and goes on.
#
# This runs the converter over a scratch tree holding one help file with
# such a line, and checks that it finishes, says what it found and where,
# and still parses the topic that follows.

import os
import shutil
import subprocess
import sys
import tempfile


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    top = os.path.abspath(os.path.join(here, '..', '..'))
    script = os.path.join(top, 'src', 'util', 'convert-help.py')
    cmdline = os.path.join(top, 'src', 'util', 'pmix_cmd_line.h')
    nfail = 0

    scratch = tempfile.mkdtemp(prefix='cvthelp.')
    try:
        util = os.path.join(scratch, 'src', 'util')
        os.makedirs(util)
        shutil.copy(cmdline, util)
        helpfile = os.path.join(util, 'help-cvtut.txt')
        with open(helpfile, 'w') as f:
            f.write('#include#help-other.txt#topic\n'
                    '[a-topic]\n'
                    'Some text.\n')
        out = os.path.join(scratch, 'out.c')

        res = subprocess.run([sys.executable, script, '--root', scratch, '--out', out],
                             capture_output=True, text=True)

        checks = [
            ('the converter finished', 0 == res.returncode),
            ('it did not raise', 'Traceback' not in res.stderr),
            ('it named the misplaced include and its file',
             'INCLUDE is specified in' in res.stderr and helpfile in res.stderr),
            ('the topic after it was still converted',
             os.path.exists(out) and 'a-topic' in open(out).read()),
        ]
        for name, ok in checks:
            print('  %s: %s' % ('PASS' if ok else 'FAIL', name))
            if not ok:
                nfail += 1
        if 0 < nfail:
            sys.stdout.write(res.stderr)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    print('check_convert_help: %d failed' % nfail)
    return 0 if 0 == nfail else 1


if __name__ == '__main__':
    sys.exit(main())
