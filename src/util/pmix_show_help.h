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
 * occur.  The SHS stores arbitrary-length help messages that are
 * looked up by filename and topic and rendered with printf()-style
 * parameters (e.g., "%s", "%d", etc.).  Note that the primary purpose
 * of the SHS is to display help messages, but it can actually be used
 * to display any arbitrary text messages.
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
 * **The "filename" is a name, not a file.** Nothing here opens
 * anything at run time, and installing a help file somewhere PMIx can
 * see it accomplishes nothing.  The help-*.txt files in the source
 * tree are read at *build* time by contrib/convert-help.py, which
 * generates src/util/pmix_show_help_content.c - a static table that is
 * compiled into libpmix - and the filename is simply the key that
 * table is indexed by.  Two consequences:
 *
 * - After adding, removing or editing any show_help content you must
 *   delete the generated pair and rebuild, or the library keeps
 *   emitting the old text:
 *
 *       rm src/util/pmix_show_help_content.* && make
 *
 *   Under --enable-devel-check the generator also runs with --purge,
 *   which makes a topic that no code references a hard build error.  So
 *   removing the last caller of a topic breaks the build for whoever
 *   regenerates next - on a fresh clone, that is CI.
 *
 * - A project outside this tree contributes its own messages by
 *   generating a table of its own and registering it with
 *   pmix_show_help_add_data(), not by installing a file.
 *
 * The format of a help file is simplistic:
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
 * A message may also pull in another one, by giving a line of the form
 * "#include#FILE#TOPIC" or "#include#PROJECT#FILE#TOPIC"; with no
 * project named, the including message's own project applies.  Include
 * chains are bounded, so a topic that includes itself stops rather than
 * running the stack out, and a directive that is not well formed is
 * skipped.
 *
 * It is expected that help messages will be grouped by filename;
 * similar messages should be in a single file.  For example, an MCA
 * component may carry its own help-*.txt, and the component can then
 * invoke pmix_show_help() to display its own help messages.
 *
 * Message files have a naming convention: they generally start with
 * the prefix "help-" and are followed by a name descriptive of what
 * kind of messages they contain.  MCA components should generally
 * abide by the MCA prefix rule, with the exception that they should
 * start the filename with "help-", as mentioned previously.
 */

#ifndef PMIX_SHOW_HELP_H
#define PMIX_SHOW_HELP_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <stdarg.h>
#include <stdatomic.h>

#include "src/include/pmix_stdatomic.h"
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
 *
 * Answers NULL when the message could not be produced - there is no
 * such filename/topic, or the rendering failed.  Note that the
 * not-found case is *not* silent: a "couldn't find that help
 * reference" notice has already been displayed by the time NULL comes
 * back, so a caller must not report the miss a second time.
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
 * Register another table of compiled-in help content, so that its
 * messages can be looked up alongside PMIx's own.
 *
 * @param project Name this content belongs to, used to disambiguate a
 * filename that appears in more than one table and as the default
 * project for an "#include" directive within it.
 * @param array Table of (filename, entries) pairs, terminated by an
 * entry with a NULL filename.  It is *borrowed*, not copied, so it must
 * outlive the show_help subsystem - a static table generated by
 * convert-help.py is the intended shape.
 *
 * Tables are searched in the order they were added, after PMIx's own.
 * A filename already claimed by another table is refused with
 * PMIX_ERROR and a message naming both projects.
 */
PMIX_EXPORT pmix_status_t pmix_show_help_add_data(const char *project,
                                                  pmix_show_help_file_t *array);

/**
 * Record that this (filename, topic) is about to be displayed, and say
 * whether it has been displayed before.
 *
 * Answers PMIX_SUCCESS if it is a duplicate - in which case it has been
 * counted, and a summary of the accumulated duplicates will be
 * displayed on a timer and again at finalize - PMIX_ERR_NOT_FOUND if
 * this is the first time, and an error otherwise.  Note that
 * PMIX_ERR_NOT_FOUND is the ordinary answer, not a failure.
 *
 * Neither argument may be NULL.  Must be called on the progress thread:
 * the duplicate list is process-global and carries no lock.
 */
PMIX_EXPORT pmix_status_t pmix_help_check_dups(const char *filename,
                                               const char *topic);

/**
 * Deliver an already-rendered show-help message.
 *
 * No lookup and no substitution is done: output is delivered as it
 * stands, and is copied, so the caller keeps ownership.  filename and
 * topic only label the message for the log.
 */
PMIX_EXPORT pmix_status_t pmix_show_help_norender(const char *filename,
                                                  const char *topic,
                                                  const char *output);

PMIX_EXPORT extern int pmix_show_help_enabled;

END_C_DECLS

#endif
