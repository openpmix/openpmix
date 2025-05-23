/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The "show help" subsystem (SHS) in PMIX is intended to help the
 * developer convey meaningful information to the user (read longer
 * than is convenient in a single printf), particularly when errors
 * occur.  The SHS allows the storage of arbitrary-length help
 * messages in text files which can be parameterized by text filename,
 * message name, POSIX locale, and printf()-style parameters (e.g.,
 * "%s", "%d", etc.).  Note that the primary purpose of the SHS is to
 * display help messages, but it can actually be used to display any
 * arbitrary text messages.
 *
 * The function pmix_show_help() is used to find a help message and
 * display it.  Its important parameters are a filename, message name,
 * and printf()-style varargs parameters used to substitute into the
 * message.
 *
 * It was originally intended that this system would support a very
 * simple version of i18n-like support, but we got (strong) feedback
 * that i18n support was not desired.  So it never happened.
 *
 * As such, the file lookup is quite straightforward -- the caller
 * passes in the filename to find the help message, and the SHS looks
 * for that file in $pkgdatadir (typically $prefix/share/pmix).
 *
 * Once the file is successfully opened, the SHS looks for the
 * appropriate help message to display.  It looks for the message name
 * in the file, reads in the message, and displays it.  printf()-like
 * substitutions are performed (e.g., %d, %s, etc.) --
 * pmix_show_help() takes a variable length argument list that are
 * used for these substitutions.
 *
 * The format of the help file is simplistic:
 *
 * - Comments begin with #.  Any characters after a # on a line are
 *   ignored.  It is not possible to escape a #.
 * - Message names are on a line by themselves and marked with [].
 *   Names can be any ASCII string within the [] (excluding the
 *   characters newline, linefeed, [, ], and #).
 * - Messages are any characters between message names and/or the end
 *   of the file.
 *
 * Here's a sample helpfile:
 *
 * \verbatimbegin
 * # This is a comment.
 * [topic 1]
 * Here's the first message.  Let's substitute in an integer: %d.
 * The quick brown fox jumped over the lazy %s.
 * # This is another comment -- it's not displayed in the first message.
 * [another:topic:foo:foo:foo]
 * This is the second message.  Let's just keep rolling along to get
 * to the second line in the message for this example.
 * \verbatimend
 *
 * It is expected that help messages will be grouped by filename;
 * similar messages should be in a single file.  For example, an MCA
 * component may install its own helpfile in PMIX's $pkgdatadir,
 * and therefore the component can invoke pmix_show_help() to display
 * its own help messages.
 *
 * Message files in $pkgdatadir have a naming convention: they
 * generally start with the prefix "help-" and are followed by a name
 * descriptive of what kind of messages they contain.  MCA components
 * should generally abide by the MCA prefix rule, with the exception
 * that they should start the filename with "help-", as mentioned
 * previously.
 */

#ifndef PMIX_SHOW_HELP_H
#define PMIX_SHOW_HELP_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <stdarg.h>

#include "src/include/pmix_globals.h"

BEGIN_C_DECLS

/**
 * \internal
 *
 * Initialization of show_help subsystem
 */
PMIX_EXPORT pmix_status_t pmix_show_help_init(void);

/**
 * \internal
 *
 * Finalization of show_help subsystem
 */
PMIX_EXPORT pmix_status_t pmix_show_help_finalize(void);

/**
 * Look up a text message in a text file and display it to the
 * stderr using printf()-like substitutions (%d, %s, etc.).
 *
 * @param filename File where the text messages are contained.
 * @param topic String index of which message to display from the
 * text file.
 * @param want_error_header Display error-bar line header and
 * footer with the message.
 * @param varargs Any additional parameters are substituted,
 * printf()-style into the help message that is displayed.
 *
 * This function looks for the filename in the $pkgdatadir
 * (typically $prefix/share/pmix), and looks up the message
 * based on the topic, and displays it.  If want_error_header is
 * true, a header and footer of asterisks are also displayed.
 *
 * Note that the "want_error_header" argument is int instead of bool,
 * because passing a parameter that undergoes default argument
 * promotion to va_start() has undefined behavior (according to clang
 * warnings on MacOS High Sierra).
 */
PMIX_EXPORT pmix_status_t pmix_show_help(const char *filename,
                                         const char *topic,
                                         int want_error_header, ...);

/**
 * This function does the same thing as pmix_show_help(), but returns
 * its output in a string (that must be freed by the caller).
 */
PMIX_EXPORT char *pmix_show_help_string(const char *filename, const char *topic,
                                        int want_error_header, ...);

/**
 * This function does the same thing as pmix_show_help_string(), but
 * accepts a va_list form of varargs.
 */
PMIX_EXPORT char *pmix_show_help_vstring(const char *filename,
                                         const char *topic,
                                         int want_error_header,
                                         va_list ap);

/**
 * This function adds another search location for the files that
 * back show_help messages. Locations will be searched starting
 * with the prefix installation directory, then cycled through
 * any additional directories in the order they were added
 */
PMIX_EXPORT pmix_status_t pmix_show_help_add_data(const char *project,
                                                  pmix_show_help_file_t *array);

// check for duplicate entries
PMIX_EXPORT pmix_status_t pmix_help_check_dups(const char *filename,
                                               const char *topic);

// output a previously rendered show-help message
PMIX_EXPORT pmix_status_t pmix_show_help_norender(const char *filename,
                                                  const char *topic,
                                                  const char *output);

PMIX_EXPORT extern bool pmix_show_help_enabled;

END_C_DECLS

#endif
