#!/usr/bin/env python3
#
# Copyright (c) 2025-2026 Jeffrey M. Squyres.  All rights reserved.
# Copyright (c) 2025-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

from __future__ import print_function
import os
import re
import sys
import argparse

def parse_cmd_line_options(path, verbose=False):
    # read the file and parse it for options
    options = []
    with open(path) as file:
        for line in file:
            stripped = line.strip()
            if stripped.startswith("#define"):
                # find the first space at end of "define"
                s1 = stripped.find(' ')
                # first word after "#define " is the name of the option
                start = 8
                end = stripped.find(' ', start+1)
                if -1 == end:
                    continue
                name = stripped[start:end]
                # find the first quote mark
                start = stripped.find('"', end)
                if start != -1:
                    # find the ending quote mark
                    end = stripped.find('"', start+1)
                    # everything in-between is the actual option
                    opt = stripped[start+1:end]
                    # track it
                    options.append((name, opt))

    return options

def is_tool_path(path, root):
    # A file belongs to a tool if it is under a "tools" directory
    # within the source tree.  Compare whole directory names in the
    # path relative to root: a substring test on the absolute path
    # also matches the directories that the source tree itself lives
    # in (e.g., a build workspace named ".../autotools-update/..."),
    # which makes every help file in the tree look like a tool's.
    dirs = os.path.dirname(os.path.relpath(path, root)).split(os.sep)
    return "tools" in dirs

def find_files(root, verbose=False):
    # Check for existence of root directory - otherwise, we just
    # fall through with no error output
    if not os.path.isdir(root):
        sys.stderr.write("Root directory " + root + " does not exist\nCannot continue\n")
        exit(1)

    # Search for help-*.txt files across the source tree, skipping
    # some directories (e.g., .git)
    # Also find and retrieve all source files (.h or .c)
    # so we can search them for entries
    help_files = []
    source_files = []
    tool_help_files = []
    tool_source_files = []
    # skip infrastructure directories
    skip_dirs = ['.git', '.libs', '.deps']
    # there may also be some help files we want to ignore
    skip_files = []
    for root_dir, dirs, files in os.walk(root):
        for sd in skip_dirs:
            if sd in dirs:
                dirs.remove(sd)

        for file in files:
            if file.startswith("help-") and file.endswith(".txt"):
                skipit = False
                for sf in skip_files:
                    if sf == file:
                        skipit = True
                        break
                if not skipit:
                    full_path = os.path.join(root_dir, file)
                    if is_tool_path(full_path, root):
                        tool_help_files.append(full_path)
                        if verbose:
                            print("Found tool help: {full_path}".format(full_path=full_path))
                    else:
                        help_files.append(full_path)
                        if verbose:
                            print("Found help: {full_path}".format(full_path=full_path))
            elif file.endswith(".c") or file.endswith(".h"):
                full_path = os.path.join(root_dir, file)
                if is_tool_path(full_path, root):
                    tool_source_files.append(full_path)
                else:
                    source_files.append(full_path)
                if verbose:
                    print("Found source: {full_path}".format(full_path=full_path))

    return help_files, source_files, tool_help_files, tool_source_files

def parse_help_files(file_paths, data, citations, verbose=False):
    # Parse INI-style help files, returning a dictionary with filenames as
    # keys.  Don't use the Python configparse module in order to
    # reduce dependencies (i.e., so that we don't have to pip install
    # anything to run this script).
    for file_path in file_paths:
        sections = {}
        current_section = None
        with open(file_path) as file:
            for line in file:
                # rstrip only: a content line's leading whitespace is part of
                # the message (help text lays its option table out in columns),
                # and stripping it also made an INDENTED line beginning with
                # '[' - e.g. a usage line reading "[e.g., host0[1-5]]" - look
                # like a section header, which silently truncated the section
                # it appeared in and put the remainder under a bogus topic.
                # Section headers start in column zero.
                stripped = line.rstrip()
                if stripped.startswith("#include"):
                    # if we aren't in a section, then this is an error
                    if current_section is None:
                        sys.stderr.write("ERROR: INCLUDE is specified in ", file_path, " outside of a section")
                        continue
                    # include the line in the section
                    sections[current_section].append(stripped)
                    # this line includes a file/topic from another file
                    # find the '#' at the end of the "include" keyword
                    start = stripped.find('#', 2)
                    if start != -1:
                        # find the '#'' at the end of the filename
                        end = stripped.find('#', start+1)
                        end2 = stripped.find('#', end+1)
                        if end2 == -1:
                            topic = stripped[end+1:]
                            fil = stripped[start+1:end]
                            citations.append((fil, topic))
                        else:
                            topic = stripped[end2+1:]
                            fil = stripped[end+1:end2]
                            citations.append((fil, topic))
                    # if a section contains an include, we treat that
                    # section as having been cited
                    citations.append((os.path.basename(file_path), current_section))
                    continue
                if stripped.startswith('#'):
                    current_section = None
                    continue
                if current_section is None and not stripped:
                    continue
                if stripped.startswith('['):
                    # find the end of the section name
                    end = stripped.find(']', 1)
                    if -1 == end:
                        continue
                    current_section = stripped[1:end]
                    # a repeated topic would silently replace the content
                    # of the earlier one, so a show_help call written
                    # against the first gets the second's text - and its
                    # format string, which need not match the caller's
                    # arguments
                    if current_section in sections:
                        sys.stderr.write("ERROR: topic [" + current_section + "] appears more than once in " + file_path + "\n")
                        exit(1)
                    sections[current_section] = list()
                elif current_section is not None:
                    sections[current_section].append(stripped)

        if file_path in data:
            sys.stderr.write("ERROR: path " + file_path + " already exists in data dictionary\n")
        else:
            data[file_path] = sections
        if verbose:
            p = len(sections)
            print("Parsed: {file_path} ({p} sections found)".format(file_path=file_path, p=p))

    return

def parse_src_files(source_files, citations, verbose=False):
    # search the source code for show_help references
    for src in source_files:
        cont_topic = False
        cont_filename = False
        with open(src) as file:
            for line in file:
                line = line.strip()

                # skip the obvious comment lines
                if line.startswith("//") or line.startswith("/*") or line.startswith('*') or line.startswith("PMIX_EXPORT"):
                    continue

                if cont_topic:
                    # the topic for the prior filename should be on this line
                    if verbose:
                        print("CONT TOPIC LINE: ", line)
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            topic = line[start+1:end]
                            citations.append((filename,topic))
                            if verbose:
                                print("Found topic: ", filename, topic)
                            cont_topic = False
                            continue
                elif cont_filename:
                    # the filename should be on this line
                    if verbose:
                        print("CONT FILENAME LINE: ", line)
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            filename = line[start+1:end]
                            # see if the topic is on the same line
                            start = line.find('"', end+1, -1)
                            if start != -1:
                                # find the end of it
                                end = line.find('"', start+1, -1)
                                if end != -1:
                                    topic = line[start+1:end]
                                    if verbose:
                                        print("Found all: ", filename, topic)
                                    citations.append((filename,topic))
                            else:
                                # topic must be on next line
                                cont_topic = True
                                if verbose:
                                    print("Found filename: ", filename)
                            cont_filename = False
                            continue
                        else:
                            cont_filename = False
                            sys.stderr.write("ERROR: Missing end of filename\n")
                            continue

                if ("pmix_show_help(" in line and not "pmix_status_t" in line) or \
                   ("pmix_show_help_string(" in line and not "char *" in line) or \
                   ("send_error_show_help(" in line and not "void" in line):
                    cont_topic = False
                    cont_filename = False
                    # line contains call to show-help - try to extract
                    # filename/topic
                    if verbose:
                        print("LINE: ", line)
                    start = line.find('"')
                    if start != -1:
                        end = line.find('"', start+1, -1)
                        if end != -1:
                            filename = line[start+1:end]
                            # see if the topic is on this line
                            start = line.find('"', end+1, -1)
                            if start != -1:
                                # find the end of it
                                end = line.find('"', start+1, -1)
                                if end != -1:
                                    topic = line[start+1:end]
                                    if verbose:
                                        print("Found all: ", filename, topic)
                                    citations.append((filename,topic))
                            else:
                                # topic must be on next line
                                cont_topic = True
                                continue
                    else:
                        # the filename must be on next line
                        cont_filename = True
                        continue

    return

def parse_tool_files(help_files, source_files, cli_options, citations, verbose=False):
    # cycle through the tool help files
    for hlp in help_files:
        topics = []
        options = []
        cli = []
        local_cli = []
        insection = False
        # scan the help file to find cmd line options that
        # are referenced in the help file itself
        with open(hlp) as file:
            for line in file:
                line = line.strip()
                if not line:
                    continue
                if line.startswith('#'):
                    insection = False
                    continue
                if line.startswith('[') and line.endswith(']'):
                    # topic - save it
                    start = line.find('[') + 1
                    end = line.find(']')
                    topic = line[start:end]
                    topics.append(topic)
                    if topic != "usage":
                        insection = True
                    continue
                if not insection:
                    # scan the line for the "--" that indicates a cmd line option
                    opt = line.find("--")
                    if -1 == opt:
                        continue
                    # opt now points to the "--" in front of the option
                    opt += 2
                    optend = line.find(' ', opt+1)
                    option = line[opt:optend]
                    # track the option
                    options.append(option)
        # get the name of the tool this help file belongs to
        d = os.path.basename(hlp)
        # find the start of the tool name
        start = d.find('-') + 1
        end = d.find('.', start)
        tool = d[start:end]
        # search the source files for the corresponding tool
        # note that we need to look at .c AND .h files for definitions
        found = False
        for src in source_files:
            f = os.path.basename(src)
            end = f.find('.')
            tsrc = f[0:end]
            if tool == tsrc:
                found = True
                # search this source for cmd line options
                with open(src) as srcfile:
                    for lsrc in srcfile:
                        lsrc = lsrc.strip()
                        if lsrc.startswith("#define PMIX_CLI"):
                            # defining a local option
                            # find the first space at end of "define"
                            s1 = lsrc.find(' ')
                            # first word after "#define " is the name of the option
                            start = 8
                            end = lsrc.find(' ', start+1)
                            if -1 == end:
                                continue
                            name = lsrc[start:end]
                            # find the first quote mark
                            start = lsrc.find('"', end)
                            if start != -1:
                                # find the ending quote mark
                                end = lsrc.find('"', start+1)
                                # everything in-between is the actual option
                                opt = lsrc[start+1:end]
                                # track it
                                local_cli.append((name, opt))

                        elif lsrc.startswith("PMIX_OPTION_") and lsrc != "PMIX_OPTION_END":
                            # the option name starts with PMIX_CLI
                            start = lsrc.find("PMIX_CLI_")
                            end = lsrc.find(",", start+1)
                            # find the option in the global list so we can
                            # compare it to the help file options
                            cli.append(lsrc[start:end])

        if not found:
            sys.stderr.write("WARNING: No corresponding source code found for help file " + hlp + "\n")
            exit(1)

        # convert the CLI to strings so we can check the options and topics
        clistrings = []
        for c in cli:
            found = False
            # check local definitions first
            for l in local_cli:
                if c == l[0]:
                    found = True
                    clistrings.append(l[1])
                    break
            if not found:
                # check the global definitions
                for copt in cli_options:
                    if c == copt[0]:
                        found = True
                        clistrings.append(copt[1])
                        break
            if not found:
                # unrecognized option
                sys.stderr.write("WARNING: Unrecognized command line option " + c + " in source code " + os.path.basename(src) + "\n")
                exit(1)

        # check the cli and options to ensure they match
        for option in options:
            found = False
            for c in clistrings:
                # there must be a CLI entry for every option
                if c == option:
                    found = True
                    break
            if not found:
                sys.stderr.write("WARNING: Option " + option + " has no corresponding CLI defined for " + tool + "\n")
                exit(1)

        # reverse must also be true
        for c in clistrings:
            # ignore "help", "version", and "verbose" as those are handled
            # by the PMIx cmd line processor
            if "help" == c or "version" == c or "verbose" == c:
                continue
            found = False
            for option in options:
                # must be an option in the help file for each CLI entry
                if c == option:
                    found = True
                    break
            if not found:
                sys.stderr.write("WARNING: CLI definition " + c + " has no corresponding help entry in " + os.path.basename(hlp) + "\n")
                exit(1)

        # also require that there be a topic for each option so that
        # "--help option" works. Note that "help", "version", and "verbose"
        # are special cases handled by the cmd line processor
        # scan the options
        for option in options:
            if option == "help" or option == "version" or option == "verbose":
                continue
            # scan the topics for a match
            found = False
            for topic in topics:
                if option == topic:
                    if verbose:
                        print("CITED ", os.path.basename(hlp), option)
                    found = True
                    citations.append((os.path.basename(hlp), option))
                    break
            if not found:
                sys.stderr.write("WARNING: Option " + option + " has no topic entry in " + os.path.basename(hlp) + "\n")


def purge(parsed_data, citations, tool_help_files):
    result_data = {}
    errorFound = False
    for filename in parsed_data:
        # tools were processed separately
        sections = parsed_data[filename]
        if filename in tool_help_files:
            result_data[filename] = sections
            continue
        result_sections = {}
        for section in sections:
            content_list = sections[section]
            # search code files for usage
            # protect special values
            if section == "help" or section == "version" or section == "usage":
                result_sections[section] = content_list
                continue
            for entry in citations:
                used = False
                file2 = entry[0]
                sec = entry[1]
                if file2 == os.path.basename(filename) and sec == section:
                    # this is a used entry
                    result_sections[section] = content_list
                    used = True
                    break;
            if not used:
                sys.stderr.write("** WARNING: Unused help topic\n")
                sys.stderr.write("    File: " + filename + "\n")
                sys.stderr.write("    Section: " + section + "\n")
                errorFound = True
        # see if anything is left
        if 0 < len(result_sections):
            # don't retain the file if no citations for it are left
            result_data[filename] = result_sections
        else:
            sys.stderr.write("File " + filename + "has no used topics - omitting\n")
            errorFound = True

    if errorFound:
        exit(1)
    return result_data

# printf conversions in a help topic; a '*' width or precision takes an
# argument of its own
FMT_SPEC = re.compile(r'%(?P<flags>[-+ #0]*)(?P<width>\*|\d+)?(?:\.(?P<prec>\*|\d+))?'
                      r'(?:hh|h|ll|l|L|z|j|t|q)?(?P<conv>[diouxXeEfFgGaAcspn%])')


def count_specs(lines):
    n = 0
    for line in lines:
        for m in FMT_SPEC.finditer(line):
            if '%' == m.group('conv'):
                continue
            n += 1
            if '*' == m.group('width'):
                n += 1
            if '*' == m.group('prec'):
                n += 1
    return n


def split_call_args(text, start):
    """Split the argument list of the call whose '(' is at text[start] at its
    top-level commas.  Returns (args, end) or (None, end) if the call cannot
    be read to its closing parenthesis."""
    depth = 0
    i = start
    args = []
    cur = []
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            # a string or character literal, escapes and all
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c in '([{':
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i
            cur.append(c)
        elif c == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return None, n


def check_citation_topics(parsed_data, citations, verbose=False):
    """Every topic a show_help call names in one of our help files must exist
    in that file.  A missing topic does not fail loudly: the lookup finds
    nothing and the user is handed the "Sorry! ... I couldn't find that
    topic" placeholder in place of the diagnostic.  Returns the number of
    missing topics found."""
    topics = {}
    for path, sections in parsed_data.items():
        topics.setdefault(os.path.basename(path), set()).update(sections.keys())
    bad = 0
    reported = set()
    for (fil, topic) in citations:
        if fil not in topics or topic in topics[fil] or (fil, topic) in reported:
            continue
        reported.add((fil, topic))
        sys.stderr.write("ERROR: show_help names a topic its help file does not have\n")
        sys.stderr.write("    File:  " + fil + "\n")
        sys.stderr.write("    Topic: " + topic + "\n")
        bad += 1
    if 0 == bad and verbose:
        print("Every topic a show_help call names exists in its help file")
    return bad


def check_call_arguments(parsed_data, source_files, verbose=False):
    """Every show_help call with a literal file and topic must pass exactly as
    many arguments as the topic has conversions.  The topic is the format
    string, so the compiler never sees it: too few arguments reads whatever
    is in the registers and on the stack, and a %s among them dereferences
    it.  Returns the number of mismatches found."""
    topics = {}
    for path, sections in parsed_data.items():
        base = os.path.basename(path)
        for topic, lines in sections.items():
            # a topic that pulls in another's text cannot be counted here
            if any(l.startswith('#include') for l in lines):
                continue
            topics[(base, topic)] = count_specs(lines)

    bad = 0
    for src in source_files:
        with open(src) as f:
            text = f.read()
        for fn in ('pmix_show_help', 'pmix_show_help_string'):
            for m in re.finditer(r'\b' + fn + r'\s*\(', text):
                args, _ = split_call_args(text, m.end() - 1)
                # filename, topic, want_error_header, then the arguments
                if args is None or len(args) < 3:
                    continue
                fname, topic = args[0], args[1]
                if not (fname.startswith('"') and topic.startswith('"')):
                    continue
                if any('__VA_ARGS__' in a for a in args):
                    continue
                key = (fname.strip('"'), topic.strip('"'))
                if key not in topics:
                    continue
                given = len(args) - 3
                want = topics[key]
                if given != want:
                    line = text.count('\n', 0, m.start()) + 1
                    sys.stderr.write("ERROR: %s:%d: %s(%s, %s) passes %d argument%s; "
                                     "the topic has %d conversion%s\n"
                                     % (src, line, fn, fname, topic, given,
                                        '' if 1 == given else 's', want,
                                        '' if 1 == want else 's'))
                    bad += 1
    if 0 == bad and verbose:
        print("Every show_help call passes as many arguments as its topic expects")
    return bad


def generate_c_code(parsed_data):
    # Generate C code with an array of filenames and their
    # corresponding INI sections.
    c_code = "// THIS FILE IS GENERATED AUTOMATICALLY! EDITS WILL BE LOST!\n// This file generated by " + os.path.basename(sys.argv[0]) + "\n\n"

    c_code += "#include \"src/include/pmix_config.h\"\n#include <stdio.h>\n#include <string.h>\n\n#include \"src/include/pmix_globals.h\"\n\n"

    ini_arrays = []
    file_entries = []

    for idx, (fil, sections) in enumerate(sorted(parsed_data.items())):
        filename = os.path.basename(fil)
        var_name = filename.replace('-', '_').replace('.', '_')

        ini_entries = []
        for section, content_list in sections.items():
            entry = "    { .topic = \"" + section + "\",\n      .content = (const char *[]){\n"
            for content in content_list:
                c_content = content.replace('"','\\"').replace("\n", '\\n"\n"').replace("\\;", ";")
                entry += "                 \"" + c_content + "\",\n"
            entry += "                 NULL}\n    },\n"
            ini_entries.append(entry)
        ini_entries.append("    { .topic = NULL, .content = NULL }")

        ini_array_name = "ini_entries_" + str(idx)
        ini_arrays.append("static pmix_show_help_entry_t " + ini_array_name + "[] = {\n" + "\n".join(ini_entries) + "\n};\n")
        file_entries.append("    { \"" + filename + "\", " + ini_array_name + "}")
    file_entries.append("    { NULL, NULL }")

    c_code += "\n".join(ini_arrays) + "\n"
    c_code += "pmix_show_help_file_t pmix_show_help_data[] = {\n" + ",\n".join(file_entries) + "\n};\n"

    return c_code

#-------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate C code from help text INI files.")
    parser.add_argument("--root",
                        required=True,
                        help="Root directory to search for help-*.txt files")
    parser.add_argument("--out",
                        help="Output C file (required unless --check-only)")
    parser.add_argument("--verbose",
                        action="store_true",
                        help="Enable verbose output")
    parser.add_argument("--purge",
                        action="store_true",
                        help="Fail on a help topic no code shows, and leave unused files out")
    parser.add_argument("--dryrun",
                        action="store_true",
                        help="Do not write out resulting ini_array_name")
    parser.add_argument("--check-only",
                        action="store_true",
                        help="Only check the show_help calls against the help files; "
                             "generate nothing")

    args = parser.parse_args()

    # The generation rule depends solely on this script, so editing a help
    # file or a call site does not re-run it.  --check-only lets "make check"
    # run the checks every time, independent of that staleness.
    if args.check_only:
        args.dryrun = True
    elif args.out is None:
        parser.error("--out is required unless --check-only is given")

    parsed_data = {}
    citations = []
    cli_options = []
    outdata = {}
    rootdir = os.path.join(args.root, "src")

    # the options are in root/util/pmix_cmd_line.h
    path = os.path.join(rootdir, "util", "pmix_cmd_line.h")
    if not os.path.exists(path):
        sys.stderr.write("File " + path + " does not exist\nCannot continue\n")
        exit(1)
    # obtain a list of (option name, string) tuples
    cli_options = parse_cmd_line_options(path, args.verbose)

    help_files, source_files, tool_help_files, tool_source_files = find_files(rootdir, args.verbose)
    parse_help_files(help_files, parsed_data, citations, args.verbose)
    parse_help_files(tool_help_files, parsed_data, citations, args.verbose)
    parse_src_files(source_files, citations, args.verbose)
    # the tools' own messages are shown from their sources
    parse_src_files(tool_source_files, citations, args.verbose)
    parse_tool_files(tool_help_files, tool_source_files, cli_options, citations, args.verbose)
    if args.check_only or args.purge:
        if check_citation_topics(parsed_data, citations, args.verbose):
            exit(1)
        if check_call_arguments(parsed_data, source_files + tool_source_files, args.verbose):
            exit(1)
    if args.check_only:
        # purge() fails on a topic no call shows
        purge(parsed_data, citations, tool_help_files)
        return
    if args.purge:
        outdata = purge(parsed_data, citations, tool_help_files)
    else:
        outdata = parsed_data

    if not args.dryrun:
        c_code = generate_c_code(outdata)

        with open(args.out, "w") as f:
            f.write(c_code)

        if args.verbose:
            print("Generated C code written to ", args.out)

if __name__ == "__main__":
    main()
