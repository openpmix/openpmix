#!/usr/bin/env python3
#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Construct a dictionary for translating attributes to/from
# their defined name and their string representation - used
# by tools to interpret user input
#

import os, os.path, sys, shutil, signal
from optparse import OptionParser, OptionGroup

def signal_handler(signal, frame):
    print("Ctrl-C received")
    sys.exit(0)

def harvest_constants(options, path, constants):
    # open the file
    try:
        inputfile = open(path, "r")
    except:
        print("File", path, "could not be opened")
        return 1

    # read the file - these files aren't too large
    # so ingest the whole thing at one gulp
    try:
        lines = inputfile.readlines()
    except:
        inputfile.close()
        return 1

    inputfile.close()  # we read everything, so done with the file
    firstline = True
    preamble = "                               \""
    linesize = 53
    # loop over the lines
    for n in range(len(lines)):
        line = lines[n]
        # remove white space at front and back
        myline = line.strip()
        # remove comment lines
        if "/*" in myline or "*/" in myline or myline.startswith("*"):
            n += 1
            continue
        # if the line starts with #define, then we want it
        if myline.startswith("#define"):
            value = myline[8:]
            # skip some well-known unwanted values
            if value.startswith("PMIx"):
                continue
            if value.startswith("PMIX_HAVE_VISIB"):
                continue
            tokens = value.split()
            if len(tokens) >= 2:
                if tokens[1][0] == '"':
                    if not firstline:
                        constants.write(",\n\n")
                    firstline = False
                    constants.write("    {.name = \"" + tokens[0] + "\", .string = " + tokens[1])
                    # only one attribute violates the one-word rule for type
                    if tokens[0] == "PMIX_EVENT_BASE":
                        dstart = 3
                        datatype = "PMIX_POINTER"
                    elif tokens[0] == "PMIX_ATTR_UNDEF":
                        constants.write(", .type = PMIX_POINTER,\n     .description = (char *[]){\"NONE\"}}")
                        continue
                    else:
                        dstart = 4
                        if tokens[3] == "(bool)":
                            datatype = "PMIX_BOOL"
                        elif tokens[3] == "(char*)":
                            datatype = "PMIX_STRING"
                        elif tokens[3] == "(pmix_rank_t)":
                            datatype = "PMIX_PROC_RANK"
                        elif tokens[3] == "(uint32_t)":
                            datatype = "PMIX_UINT32"
                        elif tokens[3] == "(int32_t)":
                            datatype = "PMIX_INT32"
                        elif tokens[3] == "(uint64_t)":
                            datatype = "PMIX_UINT64"
                        elif tokens[3] == "(int)":
                            datatype = "PMIX_INT"
                        elif tokens[3] == "(uint16_t)":
                            datatype = "PMIX_UINT16"
                        elif tokens[3] == "(pmix_data_array_t*)":
                            datatype = "PMIX_DATA_ARRAY"
                        elif tokens[3] == "(pmix_proc_t*)":
                            datatype = "PMIX_PROC"
                        elif tokens[3] == "(pmix_coord_t*)":
                            datatype = "PMIX_COORD"
                        elif tokens[3] == "(pmix_coord_view_t)":
                            datatype = "PMIX_UINT8"
                        elif tokens[3] == "(float)":
                            datatype = "PMIX_FLOAT"
                        elif tokens[3] == "(pmix_byte_object_t)":
                            datatype = "PMIX_BYTE_OBJECT"
                        elif tokens[3] == "(hwloc_topology_t)":
                            datatype = "PMIX_POINTER"
                        elif tokens[3] == "(size_t)":
                            datatype = "PMIX_SIZE"
                        elif tokens[3] == "(pmix_data_range_t)":
                            datatype = "PMIX_UINT8"
                        elif tokens[3] == "(pmix_persistence_t)":
                            datatype = "PMIX_UINT8"
                        elif tokens[3] == "(pmix_scope_t)":
                            datatype = "PMIX_UINT8"
                        elif tokens[3] == "(pmix_status_t)":
                            datatype = "PMIX_STATUS"
                        elif tokens[3] == "(void*)":
                            datatype = "PMIX_POINTER"
                        elif tokens[3] == "(TBD)":
                            datatype = "TBD"
                        elif tokens[3] == "(time_t)":
                            datatype = "PMIX_TIME"
                        elif tokens[3] == "(pmix_envar_t*)":
                            datatype = "PMIX_ENVAR"
                        elif tokens[3] == "(pid_t)":
                            datatype = "PMIX_PID"
                        elif tokens[3] == "(pmix_proc_state_t)":
                            datatype = "PMIX_PROC_STATE"
                        elif tokens[3] == "(pmix_link_state_t)":
                            datatype = "PMIX_LINK_STATE"
                        elif tokens[3] == "(pointer)":
                            datatype = "PMIX_POINTER"
                        elif tokens[3] == "(pmix_topology_t)":
                            datatype = "PMIX_TOPO"
                        elif tokens[3] == "(pmix_cpuset_t)":
                            datatype = "PMIX_PROC_CPUSET"
                        elif tokens[3] == "(pmix_geometry_t)":
                            datatype = "PMIX_GEOMETRY"
                        elif tokens[3] == "(pmix_device_distance_t)":
                            datatype = "PMIX_DEVICE_DIST"
                        elif tokens[3] == "(pmix_endpoint_t)":
                            datatype = "PMIX_ENDPOINT"
                        elif tokens[3] == "(varies)":
                            datatype = "PMIX_INT"
                        else:
                            return 1
                    constants.write(", .type = " + datatype + ",\n     .description = (char *[]){\"")
                    # the description consists of at least all remaining tokens
                    m = dstart + 1
                    desc = tokens[dstart].replace("\"", "\\\"")
                    if "DEPRECATED" in desc:
                        constants.write(desc + "\"")
                        constants.write(", NULL}}")
                        continue
                    firstout = True
                    while m < len(tokens):
                        tmp = tokens[m].replace("\"", "\\\"")
                        if (len(tmp) + len(desc) + 1) > linesize:
                            if firstout:
                                constants.write(desc + "\"")
                            else:
                                constants.write(",\n" + preamble + desc + "\"")
                            firstout = False
                            desc = tmp
                        else:
                            desc += " " + tmp
                        m += 1
                    # if the next line starts with '/', then it is a continuation
                    # of the description
                    line = lines[n+1]
                    # remove white space at front and back
                    myline = line.strip()
                    while len(myline) > 0 and myline[0] == '/':
                        # step over until we see the beginning of text
                        m = 1
                        while myline[m] == '/' or myline[m] == ' ':
                            m += 1
                        # the rest of the line is part of the description
                        k = m
                        tmp = myline[k]
                        if tmp == '"':
                            tmp = "\\\""
                        k += 1
                        while k < len(myline):
                            while k < len(myline) and ' ' != myline[k]:
                                if myline[k] == '"':
                                    tmp += "\\\""
                                else:
                                    tmp += myline[k]
                                k += 1
                            if (len(tmp) + len(desc) + 1) > linesize:
                                if firstout:
                                    constants.write(desc + "\"")
                                else:
                                    constants.write(",\n" + preamble + desc + "\"")
                                firstout = False
                                desc = tmp
                            else:
                                if len(desc) > 0:
                                    desc += " " + tmp
                                else:
                                    desc = tmp
                            tmp = ""
                            k += 1
                        n += 1
                        line = lines[n+1]
                        myline = line.strip()
                    if len(desc) > 0:
                        if firstout:
                            constants.write(desc + "\"")
                        else:
                            constants.write(",\n" + preamble + desc + "\"")
                    # finish it up by closing the definition
                    constants.write(", NULL}}")

    return 0

def main():
    signal.signal(signal.SIGINT, signal_handler)

    parser = OptionParser("usage: %prog [options]")
    debugGroup = OptionGroup(parser, "Debug Options")
    debugGroup.add_option("--dryrun",
                          action="store_true", dest="dryrun", default=False,
                          help="Show output to screen")
    parser.add_option_group(debugGroup)

    (options, args) = parser.parse_args()

    # Find the top-level PMIx source tree dir
    save = os.getcwd()
    top = os.getcwd()
    verfile = os.path.join(top, "VERSION")
    while top != "/" and not os.path.isfile(verfile):
        os.chdir("..")
        top = os.getcwd()
        verfile = os.path.join(top, "VERSION")
    if top == "/":
        print("TOP SRCDIR COULD NOT BE FOUND")
        os.chdir(save)
        return 1

    path = os.path.join(top, "include")
    os.chdir(path)

    if options.dryrun:
        constants = sys.stdout
    else:
        outpath = os.path.join(top, "dictionary.tmp")
        try:
            constants = open(outpath, "w+")
        except:
            print("DICTIONARY COULD NOT BE CONSTRUCTED")
            os.chdir(save)
            return 1

    # write the header
    constants.write("/*\n * This file is autogenerated by construct_dictionary.py.\n * Do not edit this file by hand.\n */\n\n")
    constants.write("#include \"src/include/pmix_config.h\"\n")
    constants.write("#include \"src/include/pmix_globals.h\"\n\n")

    # write the starting line
    constants.write("pmix_regattr_input_t dictionary[] = {\n")

    # scan across the header files in the src directory
    # looking for constants and typedefs
    rc = harvest_constants(options, "pmix_common.h", constants)
    if 0 != rc:
        constants.close()
        os.remove(outpath)
        print("DICTIONARY COULD NOT BE CONSTRUCTED")
        os.chdir(save)
        return 1
    constants.write(",\n")
    rc = harvest_constants(options, "pmix_deprecated.h", constants)
    if 0 != rc:
        constants.close()
        os.remove(outpath)
        print("DICTIONARY COULD NOT BE CONSTRUCTED")
        os.chdir(save)
        return 1

    # mark the end of the array
    constants.write(",\n    {.name = \"\"}\n")
    # write the closure
    constants.write("\n};\n")

    # transfer the results
    target = os.path.join(top, "src", "include", "dictionary.h")
    try:
        os.replace(outpath, target)
    except:
        print("DICTIONARY COULD NOT BE CONSTRUCTED")
        os.chdir(save)
        return 1

    # return to original directory
    os.chdir(save)

    return 0
if __name__ == '__main__':
    main()

