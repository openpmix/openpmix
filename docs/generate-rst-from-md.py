#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

"""Convert one of OpenPMIx's Markdown policy files to reStructuredText.

Files such as SECURITY.md and CODE_OF_CONDUCT.md live at the top of the
repository because that is where GitHub looks for them, and where a user
arriving at the repository looks for them.  They also belong in the online
documentation, because that is where a user who did not arrive via GitHub
looks.  Rather than maintain two copies, docs/Makefile.am runs this script
to render the Markdown source as the .rst file Sphinx includes.

Only the Markdown constructs those files actually use are supported:
ATX headings, inline and reference links, bare and angle-bracketed URLs,
inline code spans, emphasis, fenced code blocks, block quotes, bullet and
ordered lists, and pipe tables.  Anything else is passed through
unchanged, which for RST-compatible text is usually what is wanted -- but
if you reach for a construct that is not in that list, teach it to this
script rather than hand-editing its output.
"""

import argparse
import re
from pathlib import Path

HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
REF_DEF_RE = re.compile(r"^\[([^\]]+)\]:\s*(\S+)\s*$")
REF_LINK_RE = re.compile(r"\[([^\]]+)\]\[([^\]]+)\]")
INLINE_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
AUTO_LINK_RE = re.compile(r"<((?:https?|mailto):[^>]+)>")
CODE_SPAN_RE = re.compile(r"(?<!`)`([^`]+)`(?!`)")
SPAN_MARK_RE = re.compile("\x00(\\d+)\x00")
FENCE_RE = re.compile(r"^(\s*)```\s*(\S*)\s*$")
BULLET_RE = re.compile(r"^\s*[*+-]\s+")
ORDERED_RE = re.compile(r"^\s*\d+\.\s+")
QUOTE_RE = re.compile(r"^>\s?(.*)$")
TABLE_ROW_RE = re.compile(r"^\s*\|.*\|\s*$")
TABLE_SEP_RE = re.compile(r"^\s*\|[\s:|-]+\|\s*$")
UNDERLINES = ["=", "-", "~", "^", '"', "'"]


def _rst_link(text, url):
    return "`{} <{}>`_".format(text, url)


def _convert_inline(line, refs):
    """Convert the inline Markdown markup in one line of body text."""

    # Set the code spans aside first: the RST hyperlink this function
    # produces is itself delimited by backticks, so converting spans
    # afterwards would consume the link's own markup.
    spans = []

    def stash(match):
        spans.append(match.group(1))
        return "\x00{}\x00".format(len(spans) - 1)

    line = CODE_SPAN_RE.sub(stash, line)

    def unstash_plain(text):
        # RST cannot nest a literal inside a hyperlink's text, so a code
        # span used as link text becomes plain text.
        return SPAN_MARK_RE.sub(lambda m: spans[int(m.group(1))], text)

    def repl_ref(match):
        return _rst_link(unstash_plain(match.group(1)),
                         refs.get(match.group(2), match.group(2)))

    # Angle-bracketed autolinks go first: RST hyperlinks a bare URL on
    # its own, so such a link just loses its brackets -- and the links
    # produced below carry angle brackets of their own.  A mailto: keeps
    # its text form for readability.
    line = AUTO_LINK_RE.sub(lambda m: m.group(1).replace("mailto:", "", 1),
                            line)
    line = REF_LINK_RE.sub(repl_ref, line)
    line = INLINE_LINK_RE.sub(
        lambda m: _rst_link(unstash_plain(m.group(1)), m.group(2)), line)
    # A Markdown code span is one backtick; an RST literal is two.
    return SPAN_MARK_RE.sub(lambda m: "``{}``".format(spans[int(m.group(1))]),
                            line)


def _split_row(line):
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def _emit_table(rows, refs, out):
    """Render a pipe table as a list-table, header row first."""
    if out and out[-1] != "":
        out.append("")
    out.extend([".. list-table::", "   :header-rows: 1", ""])
    for row in rows:
        prefix = "   * - "
        for cell in row:
            out.append(prefix + _convert_inline(cell, refs))
            prefix = "     - "
    out.append("")


def _emit_fence(lang, body, indent, out):
    """Render a fenced code block as a code-block directive."""
    if out and out[-1] != "":
        out.append("")
    out.append("{}.. code-block:: {}".format(indent, lang if lang else "text"))
    out.append("")
    for line in body:
        out.append("{}   {}".format(indent, line) if line.strip() else "")
    out.append("")


def convert(lines, source):
    refs = {}
    for line in lines:
        match = REF_DEF_RE.match(line.strip())
        if match:
            refs[match.group(1)] = match.group(2)

    out = [
        "..",
        "   This file was generated from {} by docs/Makefile.am.".format(
            source),
        "   Do not edit directly.",
        "",
    ]

    skip_next_blank = False
    prev_bullet = False
    in_quote = False
    idx = 0
    while idx < len(lines):
        line = lines[idx]
        idx += 1

        if REF_DEF_RE.match(line.strip()):
            continue
        if skip_next_blank and not line.strip():
            skip_next_blank = False
            continue
        skip_next_blank = False

        fence = FENCE_RE.match(line)
        if fence:
            body = []
            while idx < len(lines) and not FENCE_RE.match(lines[idx]):
                body.append(lines[idx])
                idx += 1
            idx += 1  # consume the closing fence
            _emit_fence(fence.group(2), body, fence.group(1), out)
            prev_bullet = False
            continue

        if TABLE_ROW_RE.match(line):
            rows = []
            if not TABLE_SEP_RE.match(line):
                rows.append(_split_row(line))
            while idx < len(lines) and TABLE_ROW_RE.match(lines[idx]):
                if not TABLE_SEP_RE.match(lines[idx]):
                    rows.append(_split_row(lines[idx]))
                idx += 1
            _emit_table(rows, refs, out)
            in_quote = False
            prev_bullet = False
            continue

        quote = QUOTE_RE.match(line)
        if quote:
            if not in_quote and out and out[-1] != "":
                out.append("")
            out.append("   " + _convert_inline(quote.group(1), refs))
            in_quote = True
            prev_bullet = False
            continue
        in_quote = False

        match = HEADING_RE.match(line)
        if match:
            if out and out[-1] != "":
                out.append("")
            title = _convert_inline(match.group(2), refs)
            level = min(len(match.group(1)) - 1, len(UNDERLINES) - 1)
            out.extend([title, UNDERLINES[level] * len(title), ""])
            skip_next_blank = True
            prev_bullet = False
            continue

        is_item = bool(BULLET_RE.match(line)) or bool(ORDERED_RE.match(line))
        if is_item and out and out[-1] != "" and not prev_bullet:
            out.append("")
        out.append(_convert_inline(line, refs))
        prev_bullet = is_item

    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--label",
                        help="how to name the input file in the generated "
                             "file's do-not-edit header (default: its "
                             "basename)")
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        convert(input_path.read_text(encoding="utf-8").splitlines(),
                args.label if args.label else input_path.name),
        encoding="utf-8")


if __name__ == "__main__":
    main()
