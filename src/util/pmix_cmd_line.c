/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2012-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

// Local functions
static int endswith(const char *str, const char *suffix)
{
    size_t lenstr, lensuffix;

    if (NULL == str || NULL == suffix) {
        return PMIX_ERR_BAD_PARAM;
    }

    lenstr = strlen(str);
    lensuffix = strlen(suffix);
    if (lensuffix > lenstr) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 == strncmp(str + lenstr - lensuffix, suffix, lensuffix)) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERR_BAD_PARAM;
}

/* Does this token name one of the options this tool registered?
 *
 * Two places below have to tell "the user omitted my argument and what
 * follows is the next option" from "my argument merely looks option-ish".
 * Both used to answer it by testing argv[n][1] for a dash, which is wrong
 * in both directions: it rejects any value whose SECOND character is a dash
 * (so "--pmixmca hwloc_default_cpu_list 0-1" was reported as a missing
 * argument) while letting every short option through (so "--show-version -v"
 * took "-v" to be a version argument).
 *
 * Testing argv[n][0] instead is no good either - it would reject a value
 * that legitimately starts with a dash, and negative MCA values are real
 * (prte_max_vm_size and prte_progress_thread_debug_level both use -1).
 *
 * So ask the option table. A token is an option only if it actually names
 * one, which is the question we meant to ask all along.
 */
static bool is_option_token(const char *arg, const char *shorts,
                            struct option myoptions[])
{
    const char *ptr;
    size_t n, len;

    if (NULL == arg || '-' != arg[0] || '\0' == arg[1]) {
        /* not even shaped like an option - a bare "-" included, which
         * conventionally means stdin rather than an option */
        return false;
    }

    if ('-' == arg[1]) {
        /* A bare "--" separates the launcher's directives from the
         * application, so it is certainly not this option's value. */
        if ('\0' == arg[2]) {
            return true;
        }
        /* Long form. Stop at '=' so "--foo=bar" is recognized, and match on
         * a prefix, as getopt_long itself does for unambiguous
         * abbreviations. */
        ptr = arg + 2;
        len = strcspn(ptr, "=");
        for (n = 0; NULL != myoptions[n].name; n++) {
            if (0 == strncmp(ptr, myoptions[n].name, len)) {
                return true;
            }
        }
        return false;
    }

    /* Short form, possibly a cluster such as "-vvv". Every character has to
     * be a registered short option for the token to be one - that is what
     * keeps "-1" and "-n4" out. Skip the ':' argument markers in the shorts
     * string; they are modifiers, not options. */
    if (NULL == shorts) {
        return false;
    }
    for (ptr = arg + 1; '\0' != *ptr; ++ptr) {
        if (':' == *ptr || NULL == strchr(shorts, *ptr)) {
            return false;
        }
    }
    return true;
}

static void check_store(const char *name, const char *option,
                        pmix_cli_result_t *results)
{
    pmix_cli_item_t *opt;

    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        if (0 == strcmp(opt->key, name)) {
            /* if the name is NULL, then this is just setting
             * a boolean value - the presence of the option in
             * the results is considered "true" */
            if (NULL != option) {
                PMIx_Argv_append_nosize(&opt->values, option);
            }
            return;
        }
    }

    // get here if this is new option
    opt = PMIX_NEW(pmix_cli_item_t);
    opt->key = strdup(name);
    pmix_list_append(&results->instances, &opt->super);
    /* if the name is NULL, then this is just setting
     * a boolean value - the presence of the option in
     * the results is considered "true" */
    if (NULL != option) {
        PMIx_Argv_append_nosize(&opt->values, option);
    }
    return;
}

int pmix_cmd_line_parse(char **pargv, char *shorts,
                        struct option myoptions[],
                        pmix_cmd_line_store_fn_t storefn,
                        pmix_cli_result_t *results,
                        char *helpfile)
{
    int option_index = 0;   /* getopt_long stores the option index here. */
    int n, m, opt, argc, argind;
    bool found;
    char *ptr, *str, **argv;
    const char *optname;
    pmix_cmd_line_store_fn_t mystore;

    /* the getopt_long parser reorders the input argv array, so
     * we have to protect it here */
    argv = PMIx_Argv_copy(pargv);
    argc = PMIx_Argv_count(argv);
    // assign a default store_fn if one isn't provided
    if (NULL == storefn) {
        mystore = check_store;
    } else {
        mystore = storefn;
    }

    /* reset the parser - must be done each time we use it
     * to avoid hysteresis */
    optind = 0;
    opterr = 0;
    optopt = 0;
    optarg = NULL;

    if (1 == argc) {
        // nothing to parse - only the executable name is present, so
        // there is no command "tail". Note we cannot "goto done" here:
        // optind was just reset to 0, and the done: block would then
        // copy argv[0] (the executable) into results->tail, leaving
        // every caller to mistake the program name for a positional
        // argument.
        PMIx_Argv_free(argv);
        return PMIX_SUCCESS;
    }

    // run the parser
    while (1) {
        argind = optind;
        if (optind == argc || (optind > 0 && '-' != argv[optind][0])) {
            // This is the executable, or we are at the last argument.
            // Don't process any further.
            break;
        }
        opt = getopt_long(argc, argv, shorts, myoptions, &option_index);
        switch (opt) {
            case 0:
                /* if this is an MCA param of some type, store it */
                if (0 == endswith(myoptions[option_index].name, "mca")) {
                    /* format mca params as param:value - the optind value
                     * will have been incremented since the MCA param options
                     * require an argument */
                    if (NULL == argv[optind] ||
                        is_option_token(argv[optind], shorts, myoptions)) {
                        // missing the required second argument
                        str = pmix_show_help_string("help-cli.txt", "not-enough-arguments", true,
                                                    pmix_tool_basename, myoptions[option_index].name,
                                                    argv[optind-1], (NULL == argv[optind]) ?
                                                    "missing" : argv[optind],
                                                    pmix_tool_basename, myoptions[option_index].name);
                        if (NULL != str) {
                            fprintf(stderr, "%s", str);
                            fflush(stderr);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_ERR_SILENT;
                    }
                    pmix_asprintf(&str, "%s=%s", argv[optind-1], argv[optind]);
                    mystore(myoptions[option_index].name, str, results);
                    free(str);
                    ++optind;
                    break;
                }
                // if this is a "-env" option, then there can be three entries
                // that describe what to do
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_PREPEND_ENVAR) ||
                    0 == strcmp(myoptions[option_index].name, PMIX_CLI_APPEND_ENVAR)) {
                    mystore(myoptions[option_index].name, argv[optind-1], results);
                    mystore(myoptions[option_index].name, argv[optind], results);
                    ++optind;
                    break;
                }
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_UNSET_ENVAR)) {
                    // unset-env option only takes one argument
                    mystore(myoptions[option_index].name, optarg, results);
                    break;
                }
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_INFO_VERSION)) {
                    if (NULL == argv[optind] ||
                        is_option_token(argv[optind], shorts, myoptions)) {
                        // --show-version option with no args
                        mystore(myoptions[option_index].name, optarg, results);
                        break;
                    } else if (NULL != argv[optind]) {
                            if (NULL == argv[optind+1]) {
                            // one arg
                            mystore(myoptions[option_index].name, argv[optind], results);
                        } else {
                            // two args
                            mystore(myoptions[option_index].name, argv[optind], results);
                            mystore(myoptions[option_index].name, argv[optind+1], results);
                            ++optind;
                        }
                    }
                    break;
                }
                /* otherwise, store actual option */
                mystore(myoptions[option_index].name, optarg, results);
                break;
            case 'h':
                /* the "help" option can optionally take an argument. Since
                 * the argument _is_ optional, getopt will _NOT_ increment
                 * optind, so argv[optind] is the potential argument */
                if (NULL == optarg &&
                    NULL != argv[optind]) {
                    /* strip any leading dashes */
                    ptr = argv[optind];
                    while ('-' == *ptr) {
                        ++ptr;
                    }
                    // check for standard options
                    if (0 == strcmp(ptr, "version") || 0 == strcmp(ptr, "V")) {
                        str = pmix_show_help_string("help-cli.txt", "version", false);
                        if (NULL != str) {
                            printf("%s\n", str);
                            fflush(stdout);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_OPERATION_SUCCEEDED;
                    }
                    if (0 == strcmp(ptr, "verbose") || 0 == strcmp(ptr, "v")) {
                        str = pmix_show_help_string("help-cli.txt", "verbose", false);
                        if (NULL != str) {
                            printf("%s\n", str);
                            fflush(stdout);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_OPERATION_SUCCEEDED;
                    }
                    if (0 == strcmp(ptr, "help") || 0 == strcmp(ptr, "h")) {
                        // they requested help on the "help" option itself
                        str = pmix_show_help_string("help-cli.txt", "help", false,
                                                    pmix_tool_basename, pmix_tool_basename,
                                                    pmix_tool_basename, pmix_tool_basename,
                                                    pmix_tool_basename, pmix_tool_basename,
                                                    pmix_tool_basename, pmix_tool_basename);
                        if (NULL != str) {
                            printf("%s\n", str);
                            fflush(stdout);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_OPERATION_SUCCEEDED;
                    }
                    /* see if we have help on that subject */
                    str = pmix_show_help_string(helpfile, ptr, false);
                    if (NULL == str) {
                        // let the user know we don't recognize that topic
                        str = pmix_show_help_string("help-cli.txt", "unknown-option", true,
                                                    ptr, pmix_tool_basename);
                        if (NULL != str) {
                            fprintf(stderr, "%s", str);
                            fflush(stderr);
                            free(str);
                        }
                    } else {
                        printf("%s\n", str);
                        fflush(stdout);
                        free(str);
                    }
                    PMIx_Argv_free(argv);
                    return PMIX_OPERATION_SUCCEEDED;
                } else if (NULL == optarg) {
                    // high-level help request
                    str = pmix_show_help_string(helpfile, "usage", false,
                                                pmix_tool_basename, pmix_tool_org,
                                                pmix_tool_version,
                                                pmix_tool_basename,
                                                pmix_tool_msg);
                    if (NULL != str) {
                        printf("%s\n", str);
                        fflush(stdout);
                        free(str);
                    }
                    PMIx_Argv_free(argv);
                    return PMIX_OPERATION_SUCCEEDED;
                } else {  // unrecognized option
                    str = pmix_show_help_string("help-cli.txt", "unrecognized-option", true,
                                                pmix_tool_basename, optarg);
                    if (NULL != str) {
                        fprintf(stderr, "%s", str);
                        fflush(stderr);
                        free(str);
                    }
                }
                PMIx_Argv_free(argv);
                return PMIX_ERR_SILENT;
            case 'V':
                str = pmix_show_help_string(helpfile, "version", false,
                                            pmix_tool_basename, pmix_tool_org,
                                            pmix_tool_version,
                                            pmix_tool_msg);
                if (NULL != str) {
                    printf("%s\n", str);
                    fflush(stdout);
                    free(str);
                }
                // if they ask for the version, that is all we do
                PMIx_Argv_free(argv);
                return PMIX_OPERATION_SUCCEEDED;
            case 'v':
                /* Name the option from the character getopt handed back, not
                 * from option_index: getopt_long only sets option_index when
                 * it matched a LONG option, so on "-v" it still holds the
                 * last long option matched - or zero, the FIRST entry in the
                 * table, which is how the verbosity count came to be
                 * recorded against "help". */
                optname = NULL;
                for (m = 0; NULL != myoptions[m].name; m++) {
                    if (opt == myoptions[m].val) {
                        optname = myoptions[m].name;
                        break;
                    }
                }
                if (NULL == optname) {
                    // 'v' is not in this tool's table - nothing to record
                    break;
                }
                if ('-' == argv[optind-1][0] && '-' == argv[optind-1][1] &&
                    0 == strncmp(&argv[optind-1][2], optname,
                                 strlen(&argv[optind-1][2]))) {
                    /* The long form ("--verbose", or any unambiguous
                     * abbreviation of it) is a single request. It used to be
                     * dropped on the floor: this arm demanded a token
                     * beginning "-v", which "--verbose" does not. */
                    n = 1;
                } else if (0 == strncmp(argv[optind-1], "-v", 2)) {
                    /* Short form. getopt hands us 'v' once per character of
                     * a cluster ("-vvv") but only steps optind past the
                     * cluster on the last one, so the earlier calls still
                     * see the PREVIOUS argv entry - hence the test that the
                     * token is actually ours. The verbosity is then the
                     * number of v's given. */
                    n = strlen(&argv[optind-1][1]);
                } else {
                    // an interim call for a cluster we have not finished
                    break;
                }
                pmix_asprintf(&str, "%d", n);
                mystore(optname, str, results);
                free(str);
                break;
            default:
                found = false;
                for (n=0; '\0' != shorts[n]; n++) {
                    int ascii = shorts[n];
                    if (opt == ascii) {
                        /* found it - now search for matching option. The
                         * getopt fn will have already incremented optind
                         * to point at the next argument.
                         * If this short option required an argument, then
                         * it will be indicated by a ':' in the next shorts
                         * spot and the argument will be in optarg.
                         *
                         * If the short option takes an optional argument, then
                         * it will be indicated by two ':' after the option - in
                         * this case optarg will contain the argument if given.
                         * Note that the proper form of the optional argument
                         * option is "-zfoo", where 'z' is the option and "foo"
                         * is the argument. Putting a space between the option
                         * and the argument is forbidden and results in reporting
                         * of 'z' without an argument - usually followed by
                         * incorrectly marking "foo" as the beginning of the
                         * command "tail" */
                        if (':' == shorts[n+1]) {
                            // could be an optional arg
                            if (':' == shorts[n+2]) {
                                /* in this case, the argument (if given) must be immediately
                                 * attached to the option */
                                ptr = argv[optind-1];
                                ptr += 2;  // step over the '-' and option
                            } else {
                                ptr = optarg;
                            }
                        } else {
                            ptr = NULL;
                        }
                        for (m=0; NULL != myoptions[m].name; m++) {
                            if (ascii == myoptions[m].val) {
                                if (PMIX_ARG_NONE == myoptions[m].has_arg) {
                                    /* if ptr isn't NULL, then that means we were given
                                     * an argument to an option that doesn't take one.
                                     * Report the error */
                                    if (NULL != ptr) {
                                        str = pmix_show_help_string("help-cli.txt", "short-arg-error", true,
                                                                    pmix_tool_basename, shorts[n], ptr);
                                        if (NULL != str) {
                                            fprintf(stderr, "%s", str);
                                            fflush(stderr);
                                            free(str);
                                        }
                                        PMIx_Argv_free(argv);
                                        return PMIX_ERR_SILENT;
                                    }
                                    ptr = NULL;
                                } else if (0 == strcmp(myoptions[m].name, "np") &&
                                           (NULL != optarg && 0 == strcmp(optarg, "p"))) {
                                    /* we special-case the very common "-np" option */
                                    ptr = argv[optind];
                                    ++optind;
                                }
                                mystore(myoptions[m].name, ptr, results);
                                found = true;
                                break;
                            }
                        }
                        if (found) {
                            break;
                        }
                        /* this could be one of the short options other than 'h' or 'V', so
                         * we have to check */
                        if (0 != argind && '-' != argv[argind][0]) {
                            // this was not an option
                            goto done;
                        }
                        if (0 == strcmp(argv[optind-1], "--")) {
                            // double-dash indicates separator between launcher
                            // directives and the application
                            results->tail = PMIx_Argv_copy(&argv[optind]);
                            PMIx_Argv_free(argv);
                            return PMIX_SUCCESS;
                        }
                        str = pmix_show_help_string("help-cli.txt", "short-no-long", true,
                                                    pmix_tool_basename, shorts[n]);
                        if (NULL != str) {
                            fprintf(stderr, "%s", str);
                            fflush(stderr);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_ERR_SILENT;
                    }
                }
                if (found) {
                    break;
                }
                /* see if the option is in the list - if it is, then it is a
                 * "recognized" option but may be missing an argument. The
                 * getopt_long function declares these as "unrecognized", but
                 * we would like to provide a more user-friendly error message */
                for (n=0; NULL != myoptions[n].name; n++) {
                    /* skip the "--" prefix */
                    if (0 == strcmp(&argv[optind-1][2], myoptions[n].name)) {
                        /* the option is recognized - probably misssing
                         * an argument */
                        str = pmix_show_help_string("help-cli.txt", "missing-argument", true,
                                                    pmix_tool_basename, argv[optind-1],
                                                    pmix_tool_basename, &argv[optind-1][2]);
                        if (NULL != str) {
                            fprintf(stderr, "%s", str);
                            fflush(stderr);
                            free(str);
                        }
                        PMIx_Argv_free(argv);
                        return PMIX_ERR_SILENT;
                    }
                }
                if (0 == strcmp(argv[optind-1], "--")) {
                    // double-dash indicates separator between launcher
                    // directives and the application
                    goto done;
                }
                if (1 == optind) {
                    // command without any options
                    goto done;
                }
                str = pmix_show_help_string("help-cli.txt", "unregistered-option", true,
                                            pmix_tool_basename, argv[optind-1], pmix_tool_basename);
                if (NULL != str) {
                    fprintf(stderr, "%s", str);
                    fflush(stderr);
                    free(str);
                }
                PMIx_Argv_free(argv);
                return PMIX_ERR_SILENT;
        }
    }

done:
    if (optind < argc) {
        /* if this is an '&', it simply indicates that the executable
         * was being pushed into the background - ignore it */
        if (0 != strcmp(argv[optind], "&")) {
            results->tail = PMIx_Argv_copy(&argv[optind]);
        }
    }
    PMIx_Argv_free(argv);
    return PMIX_SUCCESS;
}

static void icon(pmix_cli_item_t *p)
{
    p->key = NULL;
    p->values = NULL;
}
static void ides(pmix_cli_item_t *p)
{
    if (NULL != p->key) {
        free(p->key);
    }
    if (NULL != p->values) {
        PMIx_Argv_free(p->values);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cli_item_t,
                                pmix_list_item_t,
                                icon, ides);

static void ocon(pmix_cli_result_t *p)
{
    PMIX_CONSTRUCT(&p->instances, pmix_list_t);
    p->tail = NULL;
}
static void odes(pmix_cli_result_t *p)
{
    PMIX_LIST_DESTRUCT(&p->instances);
    if (NULL != p->tail) {
        PMIx_Argv_free(p->tail);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cli_result_t,
                                pmix_object_t,
                                ocon, odes);
