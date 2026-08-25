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

/* Report an option that takes TWO tokens but was given only one.
 *
 * getopt is told these options take a single argument; the parser then
 * claims a second token from argv[optind] on its own account, so noticing
 * that the token is absent - or is the next option rather than a value -
 * is the parser's job and nobody else's. */
static void report_missing_second(const char *option, const char *first,
                                  const char *second)
{
    char *str;

    str = pmix_show_help_string("help-cli.txt", "not-enough-arguments", true,
                                pmix_tool_basename, option, first,
                                (NULL == second) ? "missing" : second,
                                pmix_tool_basename, option);
    if (NULL != str) {
        fprintf(stderr, "%s", str);
        fflush(stderr);
        free(str);
    }
    return;
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

    /* get here if this is new option. Nothing here can report a failure -
     * the store function's signature has no return - so on an allocation
     * failure drop the occurrence rather than filing an item with a NULL
     * key, which every later lookup would hand straight to strcmp(). */
    opt = PMIX_NEW(pmix_cli_item_t);
    if (NULL == opt) {
        return;
    }
    opt->key = strdup(name);
    if (NULL == opt->key) {
        PMIX_RELEASE(opt);
        return;
    }
    pmix_list_append(&results->instances, &opt->super);
    /* if the name is NULL, then this is just setting
     * a boolean value - the presence of the option in
     * the results is considered "true" */
    if (NULL != option) {
        PMIx_Argv_append_nosize(&opt->values, option);
    }
    return;
}

/* Bring every item's record of where its values were given up to date
 * with the values it actually holds.
 *
 * If "record" is set, the new values are numbered with the next
 * command-line positions; otherwise they are marked as being of unknown
 * position. The parse makes one un-recorded pass before it starts so that
 * anything a caller had already put into the results - which did not come
 * from this command line, and so has no position on it - is not later
 * mistaken for something we stored.
 *
 * Note what this does NOT do: ask the store function what it stored. The
 * store function is replaceable, and a replacement is free to file a value
 * under a different key than the one it was handed, under several keys, or
 * to drop it. Nothing here has to know: whatever grew, grew, and a value
 * an item holds beyond the extent of its seq array is by definition one
 * that was just added. That is what keeps this bookkeeping invisible to a
 * caller with a store function of its own.
 */
static void stamp_occurrences(pmix_cli_result_t *results, bool record)
{
    pmix_cli_item_t *opt;
    int n, nvals, *tmp;

    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        nvals = PMIx_Argv_count(opt->values);
        if (nvals <= opt->nseq) {
            // nothing new was stored against this option
            continue;
        }
        tmp = (int *) realloc(opt->seq, nvals * sizeof(int));
        if (NULL == tmp) {
            // out of memory - leave the positions we already have
            return;
        }
        opt->seq = tmp;
        for (n = opt->nseq; n < nvals; n++) {
            opt->seq[n] = record ? results->nseq++ : -1;
        }
        opt->nseq = nvals;
        if (0 > opt->firstseq) {
            opt->firstseq = opt->seq[0];
        }
    }
    return;
}

/* Store one option occurrence and record where on the command line it
 * was given. */
static void store_occurrence(pmix_cmd_line_store_fn_t storefn,
                             const char *name, const char *option,
                             pmix_cli_result_t *results)
{
    pmix_cli_item_t *opt;
    size_t nitems;

    nitems = pmix_list_get_size(&results->instances);
    storefn(name, option, results);
    stamp_occurrences(results, true);

    if (pmix_list_get_size(&results->instances) > nitems) {
        /* An option we had not seen before. If it was stored without a
         * value - a boolean option, whose presence is the whole of what
         * it says - then no value grew and the loop above had nothing to
         * number, so record the position on the item itself. */
        opt = (pmix_cli_item_t *) pmix_list_get_last(&results->instances);
        if (0 > opt->firstseq) {
            opt->firstseq = results->nseq++;
        }
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
    char *ptr, *str, **argv, *shortopts;
    const char *optname;
    pmix_cmd_line_store_fn_t mystore;

    /* the getopt_long parser reorders the input argv array, so
     * we have to protect it here */
    argv = PMIx_Argv_copy(pargv);
    if (NULL != pargv && NULL == argv) {
        /* the copy failed - an empty input copies to an empty array, so a
         * NULL here means we ran out of memory. Parsing nothing and
         * reporting success would silently discard the whole command line. */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    argc = PMIx_Argv_count(argv);

    /* Prefix the short-option string with '+' so getopt stops at the first
     * non-option token instead of permuting non-options to the end of argv.
     *
     * Two things depend on this. The loop below means to stop at the first
     * thing that is not an option - that token is the executable, and
     * everything from there on belongs to *it*, not to us - and it decides
     * that by looking at argv[optind]. With permutation, getopt has already
     * moved the non-options past optind by the time we look, so the loop
     * broke too late and the tail started after the very argument it was
     * supposed to start with: "prog positional --verbose" parsed the option
     * and dropped the positional entirely. And the option-name recovery
     * below reads argv[optind-1] to see which spelling the user typed,
     * which is only meaningful while argv is still in the order they typed
     * it in.
     *
     * The cost is that an option placed *after* the first non-option is no
     * longer ours - it goes to the tail. That is the intent: a launcher
     * must not eat the flags belonging to the application it launches. */
    pmix_asprintf(&shortopts, "+%s", (NULL == shorts) ? "" : shorts);
    if (NULL == shortopts) {
        PMIx_Argv_free(argv);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
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
        free(shortopts);
        return PMIX_SUCCESS;
    }

    /* Anything already in the results did not come from the command line
     * we are about to read, so it has no position on it - say so before
     * we start numbering, or the first thing we store would be credited
     * with a value someone else put there. */
    stamp_occurrences(results, false);

    // run the parser
    while (1) {
        argind = optind;
        /* Stop at the end of argv, or at the first token that is not an
         * option - that token is the executable, and everything from there
         * on belongs to it.
         *
         * The end test is ">=", not "==", because the arms below that claim
         * a second token of their own accord step optind themselves. One
         * that stepped past the end used to slip through an equality test
         * and send the next iteration reading past the end of the copied
         * array, where it dereferenced whatever it found.
         *
         * A lone "-" is not an option either - conventionally it names
         * stdin - and getopt agrees: it stops there without consuming it,
         * which left the arms below reasoning about the PREVIOUS token and
         * reporting it as missing an argument it had already been given. */
        if (optind >= argc ||
            (optind > 0 && ('-' != argv[optind][0] || '\0' == argv[optind][1]))) {
            break;
        }
        opt = getopt_long(argc, argv, shortopts, myoptions, &option_index);
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
                        report_missing_second(myoptions[option_index].name,
                                              argv[optind-1], argv[optind]);
                        PMIx_Argv_free(argv);
                        free(shortopts);
                        return PMIX_ERR_SILENT;
                    }
                    pmix_asprintf(&str, "%s=%s", argv[optind-1], argv[optind]);
                    store_occurrence(mystore, myoptions[option_index].name,
                                     str, results);
                    free(str);
                    ++optind;
                    break;
                }
                // if this is a "-env" option, then there can be three entries
                // that describe what to do
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_PREPEND_ENVAR) ||
                    0 == strcmp(myoptions[option_index].name, PMIX_CLI_APPEND_ENVAR)) {
                    /* Like the MCA options above, these take TWO tokens and
                     * getopt only knows about one, so check that the second
                     * is really there. Storing it unchecked took the NULL
                     * that terminates argv for a value and stepped optind
                     * past the end of the array. */
                    if (NULL == argv[optind] ||
                        is_option_token(argv[optind], shorts, myoptions)) {
                        report_missing_second(myoptions[option_index].name,
                                              argv[optind-1], argv[optind]);
                        PMIx_Argv_free(argv);
                        free(shortopts);
                        return PMIX_ERR_SILENT;
                    }
                    store_occurrence(mystore, myoptions[option_index].name,
                                     argv[optind-1], results);
                    store_occurrence(mystore, myoptions[option_index].name,
                                     argv[optind], results);
                    ++optind;
                    break;
                }
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_UNSET_ENVAR)) {
                    // unset-env option only takes one argument
                    store_occurrence(mystore, myoptions[option_index].name,
                                     optarg, results);
                    break;
                }
                if (0 == strcmp(myoptions[option_index].name, PMIX_CLI_INFO_VERSION)) {
                    if (NULL == argv[optind] ||
                        is_option_token(argv[optind], shorts, myoptions)) {
                        // --show-version option with no args
                        store_occurrence(mystore, myoptions[option_index].name,
                                         optarg, results);
                        break;
                    }
                    /* Takes up to two trailing tokens - a
                     * "framework[:component]" and an optional modifier (see
                     * pmix_info). Every token we claim has to be consumed:
                     * this used to advance optind by one when it took TWO
                     * and not at all when it took one, so the last argument
                     * of a --show-version was also reported back to the
                     * caller as the start of the command tail. */
                    store_occurrence(mystore, myoptions[option_index].name,
                                     argv[optind], results);
                    ++optind;
                    if (NULL != argv[optind] &&
                        !is_option_token(argv[optind], shorts, myoptions)) {
                        store_occurrence(mystore, myoptions[option_index].name,
                                         argv[optind], results);
                        ++optind;
                    }
                    break;
                }
                /* otherwise, store actual option */
                store_occurrence(mystore, myoptions[option_index].name,
                                 optarg, results);
                break;
            case 'h':
                /* The "help" option can optionally take an argument, and an
                 * optional argument reaches us two ways. Attached to the
                 * option ("-htopic", "--help=topic") getopt hands it back in
                 * optarg; given as the next token ("--help topic") it does
                 * NOT step optind, so the token is still at argv[optind].
                 *
                 * Both spell the same request, and both have to be answered
                 * the same way. The attached form used to fall through to an
                 * "unrecognized option" complaint naming the topic, so
                 * "--help=version" was refused while "--help version"
                 * worked. */
                ptr = (NULL != optarg) ? optarg : argv[optind];
                if (NULL != ptr) {
                    /* strip any leading dashes */
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
                        free(shortopts);
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
                        free(shortopts);
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
                        free(shortopts);
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
                    free(shortopts);
                    return PMIX_OPERATION_SUCCEEDED;
                }
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
                free(shortopts);
                return PMIX_OPERATION_SUCCEEDED;
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
                free(shortopts);
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
                store_occurrence(mystore, optname, str, results);
                free(str);
                break;
            default:
                found = false;
                /* A tool with no short options at all passes shorts == NULL,
                 * which the '+'-prefixing above and is_option_token() both
                 * allow for - so this scan has to as well. */
                for (n=0; NULL != shorts && '\0' != shorts[n]; n++) {
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
                                        free(shortopts);
                                        return PMIX_ERR_SILENT;
                                    }
                                    ptr = NULL;
                                } else if (0 == strcmp(myoptions[m].name, "np") &&
                                           (NULL != optarg && 0 == strcmp(optarg, "p"))) {
                                    /* We special-case the very common "-np"
                                     * option: getopt read the "p" as the
                                     * argument of "-n", so the count is the
                                     * token after it. It has to be there -
                                     * claiming the NULL that terminates argv
                                     * stepped optind past the end of the
                                     * array, and the next iteration then
                                     * read past it. */
                                    if (NULL == argv[optind]) {
                                        report_missing_second(myoptions[m].name,
                                                              "-np", NULL);
                                        PMIx_Argv_free(argv);
                                        free(shortopts);
                                        return PMIX_ERR_SILENT;
                                    }
                                    ptr = argv[optind];
                                    ++optind;
                                }
                                store_occurrence(mystore, myoptions[m].name, ptr, results);
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
                            free(shortopts);
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
                        free(shortopts);
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
                    /* Skip the "--" prefix. Only a token that actually has
                     * one can be indexed past it: getopt hands back the
                     * offending option here, but the arms that reach this
                     * point on a -1 return are looking at whatever token
                     * preceded it, which need not be that long. */
                    if ('-' != argv[optind-1][0] || '-' != argv[optind-1][1]) {
                        break;
                    }
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
                        free(shortopts);
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
                free(shortopts);
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
    free(shortopts);
    return PMIX_SUCCESS;
}

/* Is occurrence "a" to be reported after occurrence "b"?
 *
 * A value whose position was never recorded (seq < 0) has no place in the
 * ordering, so it goes after everything that does have one. */
static bool occurs_after(const pmix_cli_occurrence_t *a,
                         const pmix_cli_occurrence_t *b)
{
    if (0 > a->seq) {
        return (0 <= b->seq);
    }
    if (0 > b->seq) {
        return false;
    }
    return (a->seq > b->seq);
}

int pmix_cmd_line_get_ordered(pmix_cli_result_t *results,
                              pmix_cli_occurrence_t **occurrences,
                              size_t *nocc)
{
    pmix_cli_item_t *opt;
    pmix_cli_occurrence_t *array, tmp;
    size_t n, m, num;
    int i, nvals;

    if (NULL == results || NULL == occurrences || NULL == nocc) {
        return PMIX_ERR_BAD_PARAM;
    }
    *occurrences = NULL;
    *nocc = 0;

    // how many occurrences are there to report?
    num = 0;
    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        nvals = PMIx_Argv_count(opt->values);
        /* an option given without a value still occurred, and is
         * reported as one entry carrying no value */
        num += (0 == nvals) ? 1 : (size_t) nvals;
    }
    if (0 == num) {
        return PMIX_SUCCESS;
    }
    array = (pmix_cli_occurrence_t *) malloc(num * sizeof(pmix_cli_occurrence_t));
    if (NULL == array) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    n = 0;
    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        nvals = PMIx_Argv_count(opt->values);
        if (0 == nvals) {
            array[n].key = opt->key;
            array[n].value = NULL;
            array[n].seq = opt->firstseq;
            ++n;
            continue;
        }
        for (i = 0; i < nvals; i++) {
            array[n].key = opt->key;
            array[n].value = opt->values[i];
            array[n].seq = (i < opt->nseq) ? opt->seq[i] : -1;
            ++n;
        }
    }

    /* Sort into the order the options were given. An insertion sort, and
     * not qsort, because it is stable: the entries with no recorded
     * position are all "equal" and there is no better order to give them
     * than the one they came in with. A command line is a handful of
     * options long, so the cost of that choice is nothing. */
    for (n = 1; n < num; n++) {
        tmp = array[n];
        m = n;
        while (0 < m && occurs_after(&array[m - 1], &tmp)) {
            array[m] = array[m - 1];
            --m;
        }
        array[m] = tmp;
    }

    *occurrences = array;
    *nocc = num;
    return PMIX_SUCCESS;
}

static void icon(pmix_cli_item_t *p)
{
    p->key = NULL;
    p->values = NULL;
    p->seq = NULL;
    p->nseq = 0;
    p->firstseq = -1;
}
static void ides(pmix_cli_item_t *p)
{
    if (NULL != p->key) {
        free(p->key);
    }
    if (NULL != p->values) {
        PMIx_Argv_free(p->values);
    }
    if (NULL != p->seq) {
        free(p->seq);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cli_item_t,
                                pmix_list_item_t,
                                icon, ides);

static void ocon(pmix_cli_result_t *p)
{
    PMIX_CONSTRUCT(&p->instances, pmix_list_t);
    p->tail = NULL;
    p->nseq = 0;
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
