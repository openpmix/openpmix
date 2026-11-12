/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_IOF_WRITER_H
#define PMIX_IOF_WRITER_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

BEGIN_C_DECLS

/* The output files a privileged server opens for another user are
 * written by a writer thread, not the progress thread: a regular file
 * ignores O_NONBLOCK, so a write to one on a filesystem that does not
 * respond blocks the thread making it.
 *
 * The server writes to the non-blocking end of a pipe instead, and one writer
 * thread copies from the pipe to the file. A file that hangs holds only
 * that thread; what the server has queued meanwhile is bounded (see
 * pmix_iof_writer_backlog). The thread is started by the first file that
 * needs it, so a server that writes no other user's files has none.
 *
 * pmix_iof_writer_relay() takes fd, a file opened as owner, and returns
 * the descriptor to write to instead: the write end of the pipe,
 * non-blocking and close-on-exec. The writer thread owns fd from then on,
 * checks it - a regular file with one name, owned by owner - before it
 * writes any of it, and closes it when the pipe is closed. "what" names
 * the file in what it logs. On failure fd is closed and -1 returned.
 * Progress thread only. */
PMIX_EXPORT int pmix_iof_writer_relay(int fd, uid_t owner, const char *what);

/* Output queued for one such file, in chunks, past which more is dropped
 * rather than held - MCA iof_writer_backlog */
PMIX_EXPORT extern size_t pmix_iof_writer_backlog;

/* Let the writer thread finish what it has and stop, waiting no longer
 * than runas_timeout for it. One held by a file that does not answer
 * is left running, and is reused if the library is initialized again */
PMIX_EXPORT void pmix_iof_writer_finalize(void);

/* TESTING ONLY. Set, the writer thread copies nothing until it is
 * cleared - which is what a file on a filesystem that does not answer
 * looks like from the server. test/unit/iof_file_perms.c only */
PMIX_EXPORT extern volatile bool pmix_iof_writer_test_stall;

END_C_DECLS

#endif
