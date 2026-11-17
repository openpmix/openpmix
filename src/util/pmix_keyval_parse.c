/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The parser for MCA parameter files.
 *
 * The format is line-oriented and every line stands on its own, so this
 * reads one line at a time and classifies it.  A line is one of:
 *
 *     # comment            // comment          / * block comment * /
 *     name = value         (value is the rest of the line, verbatim)
 *     -mca name value      (--mca too; the value may be quoted)
 *     -x NAME=VALUE        (--x too)
 *     -x NAME
 *
 * Only the first two forms are documented (see docs/mca.rst); the "-mca"
 * and "-x" spellings, and the "//" and block comment forms, are not, but
 * they have always been accepted here and a file in the wild may use
 * them, so they keep working.
 *
 * This used to be driven by a flex scanner.  Nothing above needs one -
 * there is no nesting and no lookahead past the end of a line - and the
 * scanner's longest-match and trailing-context rules were the direct
 * cause of four silent misreadings: a malformed name resumed parsing in
 * the middle of its own line and set some *other* parameter, a CRLF file
 * left a carriage return on the end of every value, a quoted "-mca"
 * value that ended a line was truncated at the first space inside the
 * quotes, and a bare "-x NAME" at the end of a file with no closing
 * newline lost its last character.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_DLFCN_H
#    include <dlfcn.h>
#endif
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pmix_common.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_output.h"

int pmix_util_keyval_parse_lineno = 0;

static pmix_mutex_t keyval_mutex;

static char *env_str = NULL;
#define PMIX_KEYVAL_ENV_STR_INIT 1024
static size_t envsize = PMIX_KEYVAL_ENV_STR_INIT;
/* strlen(env_str), kept so an append need not measure the string */
static size_t envlen = 0;
/* false while parsing a stream - a file read for someone else, whose
 * -x directives do not belong in this process's list */
static bool collect_env = true;

/* Nothing in a parameter file is length-limited, so the line buffer
 * starts small and grows to fit the longest line in the file. */
#define PMIX_KEYVAL_LINE_INIT 256

void pmix_util_keyval_parse_finalize(void)
{
    /* env_str accumulates the -x directives of every file parsed, and is
     * normally handed off - and freed - by
     * pmix_util_keyval_save_internal_envars().  That hand-off does not
     * always happen: pmix_mca_base_var_cache_files() returns on the first
     * file that fails to parse, before it reaches the store.  Leaving the
     * string standing is worse than a leak, because everything in this
     * file is process-global and pmix_init_util() runs again after
     * pmix_finalize_util() - a surviving env_str hands the next run the
     * previous run's variables. */
    free(env_str);
    env_str = NULL;
    envsize = PMIX_KEYVAL_ENV_STR_INIT;
    envlen = 0;

    PMIX_DESTRUCT(&keyval_mutex);
}

int pmix_util_keyval_parse_init(void)
{
    PMIX_CONSTRUCT(&keyval_mutex, pmix_mutex_t);

    return PMIX_SUCCESS;
}

/*
 * Character classes.  Both are spelled out rather than handed to
 * isspace()/isalnum() so that what a parameter file means cannot depend
 * on the locale the process happens to be running in.
 */
static bool is_white(char c)
{
    return (' ' == c || '\t' == c || '\f' == c || '\v' == c);
}

static bool is_key_char(char c)
{
    return (('a' <= c && 'z' >= c) || ('A' <= c && 'Z' >= c)
            || ('0' <= c && '9' >= c) || '_' == c || '-' == c || '.' == c);
}

static char *skip_white(char *p)
{
    while (is_white(*p)) {
        p++;
    }
    return p;
}

static void parse_error(const char *filename, int lineno, const char *line,
                        const char *why)
{
    pmix_output(0, "keyval parser: %s: line %d: %s\n  %s", filename, lineno, why,
                line);
}

/*
 * Grow *buf to hold at least "needed" bytes.  A failure leaves the
 * existing buffer intact so the caller can still report the error.
 */
static int ensure_room(char **buf, size_t *len, size_t needed)
{
    char *tmp;
    size_t newlen;

    if (needed <= *len) {
        return PMIX_SUCCESS;
    }

    newlen = (0 == *len) ? PMIX_KEYVAL_LINE_INIT : *len;
    while (newlen < needed) {
        newlen *= 2;
    }

    tmp = (char *) realloc(*buf, newlen);
    if (NULL == tmp) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    *buf = tmp;
    *len = newlen;

    return PMIX_SUCCESS;
}

/*
 * Read one line into *buf, growing it as needed, and strip the line
 * terminator - LF, and the CR of a CRLF pair, so that a file written on
 * Windows does not leave a carriage return on the end of every value.
 *
 * A final line with no terminator at all is still a line: an editor that
 * does not add one, a here-doc and a generated file all produce them.
 *
 * Every byte read is charged against *budget; a read that would take it
 * below zero ends the parse, so a stream cannot grow the line buffer, or
 * run the parse, past what its caller allowed.
 *
 * @return PMIX_SUCCESS when a line was read, PMIX_ERR_NOT_FOUND at end
 *         of file, PMIX_ERR_OUT_OF_RESOURCE if the buffer could not grow,
 *         PMIX_ERR_BAD_PARAM if the stream is larger than *budget.
 */
static int read_line(FILE *fp, char **buf, size_t *buflen, size_t *budget)
{
    size_t used = 0;
    size_t chunk;
    int rc;

    for (;;) {
        rc = ensure_room(buf, buflen, used + PMIX_KEYVAL_LINE_INIT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

        if (NULL == fgets(*buf + used, (int) (*buflen - used), fp)) {
            /* end of file, or a read error.  Either way, what we have
             * collected so far is the last line. */
            break;
        }

        chunk = strlen(*buf + used);
        if (0 == chunk) {
            /* fgets stopped on a NUL byte, so it cannot tell us how far
             * it actually got.  A parameter file is text; treat this as
             * the end of the line rather than spinning here. */
            break;
        }
        if (chunk > *budget) {
            return PMIX_ERR_BAD_PARAM;
        }
        *budget -= chunk;
        used += chunk;

        if ('\n' == (*buf)[used - 1]) {
            break;
        }
        /* the buffer filled before the line ended - go round again */
    }

    if (0 == used) {
        return PMIX_ERR_NOT_FOUND;
    }

    if ('\n' == (*buf)[used - 1]) {
        used--;
    }
    if (0 < used && '\r' == (*buf)[used - 1]) {
        used--;
    }
    (*buf)[used] = '\0';

    return PMIX_SUCCESS;
}

/*
 * If *pp opens with the directive named by \c name - with one or two
 * leading dashes, and followed by whitespace - return the position just
 * past it, else NULL.  The trailing whitespace is what keeps "-x" from
 * matching the front of "-xyz".
 */
static char *match_directive(char *p, const char *name)
{
    size_t len = strlen(name);

    if ('-' != *p) {
        return NULL;
    }
    p++;
    if ('-' == *p) {
        p++;
    }
    if (0 != strncmp(p, name, len)) {
        return NULL;
    }
    p += len;
    if (!is_white(*p)) {
        return NULL;
    }

    return p;
}

/*
 * Take the next run of name characters from *pp, NUL-terminating it in
 * place.  The character that ended it is handed back through \c delim,
 * because it is read before the NUL overwrites it and the caller needs
 * to know whether it was an '=' - that is what separates "-x FOO=bar"
 * from "-x FOO".
 *
 * @return the name, or NULL if there was not one.
 */
static char *next_name(char **pp, char *delim)
{
    char *p = skip_white(*pp);
    char *start = p;

    while (is_key_char(*p)) {
        p++;
    }
    if (p == start) {
        *pp = p;
        return NULL;
    }

    *delim = *p;
    if ('\0' != *p) {
        *p = '\0';
        p++;
    }
    *pp = p;

    return start;
}

/*
 * Take the next token from *pp, NUL-terminating it in place.  A token
 * runs to the next whitespace, except that one which opens with a quote
 * runs to the matching quote and is delivered without the quotes -
 * whether or not anything follows it on the line.
 *
 * @return the token, or NULL at the end of the line.
 */
static char *next_token(char **pp)
{
    char *p = skip_white(*pp);
    char *start;
    char quote;

    if ('\0' == *p) {
        *pp = p;
        return NULL;
    }

    if ('\'' == *p || '"' == *p) {
        quote = *p;
        start = ++p;
        while ('\0' != *p && quote != *p) {
            p++;
        }
        if ('\0' == *p) {
            /* an unterminated quote takes the rest of the line */
            *pp = p;
            return start;
        }
        *p = '\0';
        *pp = p + 1;
        return start;
    }

    start = p;
    while ('\0' != *p && !is_white(*p)) {
        p++;
    }
    if ('\0' != *p) {
        *p = '\0';
        p++;
    }
    *pp = p;

    return start;
}

static int add_to_env_str(const char *var, const char *val)
{
    size_t varsz, valsz = 0, sz, new_envsize;
    char *tmp, *p;

    if (NULL == var) {
        return PMIX_ERR_BAD_PARAM;
    }

    varsz = strlen(var);
    if (NULL != val) {
        /* account for '=' */
        valsz = strlen(val) + 1;
    }
    /* what is there, a ';' if anything is, the new entry and a NUL */
    sz = envlen + ((0 < envlen) ? 1 : 0) + varsz + valsz + 1;

    /* make sure we have sufficient space */
    new_envsize = envsize;
    while (new_envsize < sz) {
        new_envsize *= 2;
    }

    if (NULL == env_str) {
        env_str = (char *) malloc(new_envsize);
        if (NULL == env_str) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        envlen = 0;
    } else if (new_envsize > envsize) {
        tmp = (char *) realloc(env_str, new_envsize);
        if (NULL == tmp) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        env_str = tmp;
    }
    envsize = new_envsize;

    /* append at the end we know rather than searching for it */
    p = env_str + envlen;
    if (0 < envlen) {
        *p++ = ';';
    }
    memcpy(p, var, varsz);
    p += varsz;
    if (NULL != val) {
        *p++ = '=';
        memcpy(p, val, valsz - 1);
        p += valsz - 1;
    }
    *p = '\0';
    envlen = (size_t) (p - env_str);

    return PMIX_SUCCESS;
}

/*
 * A "name = value" line.  The name is a run of name characters; the
 * value is everything to the right of the '=' with the whitespace
 * trimmed off both ends and nothing else touched - docs/mca.rst promises
 * that quotes and '#' inside a value are part of it.
 *
 * A malformed line is reported and dropped *whole*.  Resuming in the
 * middle of one is how "a:b = 1" used to quietly set the parameter "b".
 */
static int parse_assignment(const char *filename, int lineno, char *p,
                            const char *original, pmix_keyval_parse_fn_t callback,
                            void *cbdata)
{
    char *eq, *keyend, *value, *c;

    eq = strchr(p, '=');
    if (NULL == eq) {
        parse_error(filename, lineno, original, "no '=' on the line");
        return PMIX_SUCCESS;
    }

    keyend = eq;
    while (keyend > p && is_white(keyend[-1])) {
        keyend--;
    }
    if (keyend == p) {
        parse_error(filename, lineno, original, "no parameter name before the '='");
        return PMIX_SUCCESS;
    }
    for (c = p; c < keyend; c++) {
        if (!is_key_char(*c)) {
            parse_error(filename, lineno, original,
                        "a parameter name may hold only letters, digits, '_', '-' and '.'");
            return PMIX_SUCCESS;
        }
    }
    /* this may overwrite the '=' itself, which is why the value is taken
     * from one past where it was rather than from one past keyend */
    *keyend = '\0';

    value = skip_white(eq + 1);
    c = value + strlen(value);
    while (c > value && is_white(c[-1])) {
        c--;
    }
    *c = '\0';

    pmix_util_keyval_parse_lineno = lineno;
    callback(filename, lineno, p, ('\0' == *value) ? NULL : value, cbdata);

    return PMIX_SUCCESS;
}

/*
 * One or more "-mca name value" / "-x NAME[=VALUE]" directives, which
 * may share a line.  A malformed one takes the rest of the line with it.
 */
static int parse_directives(const char *filename, int lineno, char *p,
                            const char *original, pmix_keyval_parse_fn_t callback,
                            void *cbdata)
{
    char *q, *name, *value;
    char delim;
    int rc;

    for (;;) {
        p = skip_white(p);
        if ('\0' == *p) {
            return PMIX_SUCCESS;
        }

        if (NULL != (q = match_directive(p, "mca"))) {
            p = q;
            name = next_name(&p, &delim);
            if (NULL == name) {
                parse_error(filename, lineno, original, "-mca with no parameter name");
                return PMIX_SUCCESS;
            }
            value = next_token(&p);
            if (NULL == value) {
                parse_error(filename, lineno, original, "-mca with no value");
                return PMIX_SUCCESS;
            }
            pmix_util_keyval_parse_lineno = lineno;
            callback(filename, lineno, name, value, cbdata);
            continue;
        }

        if (NULL != (q = match_directive(p, "x"))) {
            p = q;
            name = next_name(&p, &delim);
            if (NULL == name) {
                parse_error(filename, lineno, original, "-x with no variable name");
                return PMIX_SUCCESS;
            }
            /* "-x FOO = bar" is as good as "-x FOO=bar", so if the name
             * did not end on the '=' itself, look past the whitespace
             * for one before concluding there is no value */
            if ('=' != delim) {
                q = skip_white(p);
                if ('=' == *q) {
                    p = q + 1;
                    delim = '=';
                }
            }
            value = ('=' == delim) ? next_token(&p) : NULL;

            /* the -x directives are not delivered as they are read: they
             * pile up in one string that is handed over by
             * pmix_util_keyval_save_internal_envars(). A stream is a file
             * read for someone else, and its directives are not ours */
            if (collect_env) {
                rc = add_to_env_str(name, value);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            }
            continue;
        }

        parse_error(filename, lineno, original,
                    "expected another -mca or -x directive");
        return PMIX_SUCCESS;
    }
}

/*
 * Classify one line.  \c work is the line, which is consumed in place;
 * \c original is an untouched copy of it, used only to quote the line
 * back in an error message.
 */
static int process_line(const char *filename, int lineno, char *work,
                        const char *original, bool *in_comment,
                        pmix_keyval_parse_fn_t callback, void *cbdata)
{
    char *p = work;
    char *end;

    if (*in_comment) {
        end = strstr(p, "*/");
        if (NULL == end) {
            return PMIX_SUCCESS;
        }
        *in_comment = false;
        p = end + 2;
    }

    for (;;) {
        p = skip_white(p);
        if ('\0' == *p || '#' == *p) {
            return PMIX_SUCCESS;
        }
        if ('/' == p[0] && '/' == p[1]) {
            return PMIX_SUCCESS;
        }
        if ('/' == p[0] && '*' == p[1]) {
            end = strstr(p + 2, "*/");
            if (NULL == end) {
                *in_comment = true;
                return PMIX_SUCCESS;
            }
            p = end + 2;
            continue;
        }
        break;
    }

    if (NULL != match_directive(p, "mca") || NULL != match_directive(p, "x")) {
        return parse_directives(filename, lineno, p, original, callback, cbdata);
    }

    return parse_assignment(filename, lineno, p, original, callback, cbdata);
}

/* read fp to its end, or until maxbytes (0 for no limit) have been read.
 * Called with keyval_mutex held */
static int parse_stream(FILE *fp, const char *filename, size_t maxbytes,
                        pmix_keyval_parse_fn_t callback, void *cbdata)
{
    size_t budget = (0 == maxbytes) ? SIZE_MAX : maxbytes;
    char *line = NULL;
    char *work = NULL;
    size_t linelen = 0;
    size_t worklen = 0;
    bool in_comment = false;
    int lineno = 0;
    int rc = PMIX_SUCCESS;
    int ret = PMIX_SUCCESS;

    while (PMIX_SUCCESS == (rc = read_line(fp, &line, &linelen, &budget))) {
        char *start = line;
        size_t len;

        lineno++;
        /* an editor that writes a UTF-8 byte-order mark puts three bytes
         * in front of the first name.  They are not part of it. */
        if (1 == lineno && 0 == strncmp(start, "\xef\xbb\xbf", 3)) {
            start += 3;
        }

        len = strlen(start);
        rc = ensure_room(&work, &worklen, len + 1);
        if (PMIX_SUCCESS != rc) {
            ret = rc;
            break;
        }
        memcpy(work, start, len + 1);

        rc = process_line(filename, lineno, work, start, &in_comment, callback,
                          cbdata);
        /* A malformed line is reported to the user by parse_error() and the
         * rest of the file is still read - that is long-standing behavior
         * and callers depend on a typo not aborting startup.  Running out
         * of memory is different: the line was dropped without a word to
         * anyone, nothing after it can succeed, and our caller
         * (pmix_mca_base_var_cache_files) acts on this return code. */
        if (PMIX_ERR_OUT_OF_RESOURCE == rc) {
            ret = rc;
            break;
        }
    }
    if (PMIX_ERR_OUT_OF_RESOURCE == rc || PMIX_ERR_BAD_PARAM == rc) {
        ret = rc;
    }

    free(line);
    free(work);
    return ret;
}

int pmix_util_keyval_parse_stream(FILE *fp, const char *filename, size_t maxbytes,
                                  pmix_keyval_parse_fn_t callback, void *cbdata)
{
    int ret;

    if (NULL == fp || NULL == filename) {
        return PMIX_ERR_BAD_PARAM;
    }
    pmix_mutex_lock(&keyval_mutex);
    collect_env = false;
    ret = parse_stream(fp, filename, maxbytes, callback, cbdata);
    collect_env = true;
    pmix_mutex_unlock(&keyval_mutex);
    return ret;
}

/*
 * Who could have written a parameter file. A file sets any parameter it
 * names, so it is read only if nobody could have written it but root, the
 * user we run as, and - for a process not running as root - the members of
 * a group it belongs to. That is decided for the file, on the descriptor
 * actually opened, and for the directory holding it, since whoever can
 * write a directory can replace what is in it; a sticky directory leaves a
 * file to its owner, which the file's own test decides. A symbolic link is
 * followed - parameter files are commonly linked into place - and then the
 * directory of the file it resolves to is tested as well as the
 * directory holding the link.
 *
 * A process running as root takes no group's word: the members of a group
 * root belongs to can be ordinary users.
 */

/* Is gid one of ours? The kernel holds our groups, so this needs no
 * lookup and works however early it is asked. */
static bool our_group(gid_t gid)
{
    gid_t *groups;
    int n, i;
    bool found = false;

    if (gid == getegid()) {
        return true;
    }
    n = getgroups(0, NULL);
    if (0 >= n) {
        return false;
    }
    groups = (gid_t *) malloc((size_t) n * sizeof(gid_t));
    if (NULL == groups) {
        return false;
    }
    n = getgroups(n, groups);
    for (i = 0; i < n; i++) {
        if (groups[i] == gid) {
            found = true;
            break;
        }
    }
    free(groups);
    return found;
}

/*
 * The users who could have changed the code this process runs.
 *
 * A parameter file may be owned by root, by us, or by one of these: the
 * owner of the file holding the PMIx library we have loaded - or of the
 * executable, when the library is linked into it - and the owner of each
 * directory above it, since the owner of a directory can replace what is
 * in it. Each of them could have replaced that library before we started,
 * so a parameter file one of them wrote changes nothing they could not
 * already have changed. That admits the files of an installation made by
 * one user and used by others, and a build tree run by root, without
 * admitting anyone new.
 *
 * Only a path that names the file actually mapped is used. On Linux that
 * is the kernel's own, from /proc/self/maps: dladdr() there reports
 * argv[0] for a symbol in the main program, and argv[0] is whatever our
 * caller chose. Elsewhere it is dladdr()'s, if absolute, and the path it
 * resolves to as well. When neither can be had, the set is empty and only
 * root and we may own a parameter file. Owners only: the group and mode of
 * those directories add nobody.
 *
 * Found once, when a file owned by somebody else first needs it; called
 * with keyval_mutex held.
 */
#define PMIX_KEYVAL_MAX_CODE_OWNERS 32
static uid_t code_owners[PMIX_KEYVAL_MAX_CODE_OWNERS];
static int ncode_owners = 0;
static bool code_owners_found = false;
/* an address inside this library: initialized read-only data, so it is
 * mapped from the library's file and not anonymous memory */
static const char code_owners_marker = 1;

static void add_code_owner(uid_t uid)
{
    int i;

    if (0 == uid || geteuid() == uid) {
        return;
    }
    for (i = 0; i < ncode_owners; i++) {
        if (code_owners[i] == uid) {
            return;
        }
    }
    /* past the limit, an owner is simply not admitted */
    if (PMIX_KEYVAL_MAX_CODE_OWNERS > ncode_owners) {
        code_owners[ncode_owners++] = uid;
    }
}

/* the owners of path and of every directory above it */
static void add_path_owners(const char *path)
{
    char buf[PATH_MAX];
    struct stat st;
    char *slash;

    if ('/' != path[0] || sizeof(buf) <= strlen(path)) {
        return;
    }
    strcpy(buf, path);
    while (1) {
        if (0 == lstat(buf, &st)) {
            add_code_owner(st.st_uid);
        }
        slash = strrchr(buf, '/');
        if (NULL == slash) {
            break;
        }
        if (slash == buf) {
            if ('\0' == buf[1]) {
                break;
            }
            buf[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

#if defined(__linux__)
/* the file mapped at addr, as the kernel names it */
static bool mapped_file(const void *addr, char *path, size_t len)
{
    FILE *fp;
    char line[PATH_MAX + 128];
    unsigned long start, end, inode;
    int n;
    char *name, *nl;
    bool found = false;

    fp = fopen("/proc/self/maps", "re");
    if (NULL == fp) {
        return false;
    }
    while (NULL != fgets(line, sizeof(line), fp)) {
        n = 0;
        if (3 != sscanf(line, "%lx-%lx %*s %*s %*s %lu %n", &start, &end, &inode, &n) ||
            0 == n) {
            continue;
        }
        if ((uintptr_t) addr < start || (uintptr_t) addr >= end) {
            continue;
        }
        name = line + n;
        if (NULL != (nl = strchr(name, '\n'))) {
            *nl = '\0';
        }
        /* a file since removed or replaced names nothing we can check */
        if (0 != inode && '/' == name[0] && NULL == strstr(name, " (deleted)") &&
            len > strlen(name)) {
            strcpy(path, name);
            found = true;
        }
        break;
    }
    fclose(fp);
    return found;
}
#endif

static void find_code_owners(void)
{
    code_owners_found = true;
#if defined(__linux__)
    {
        char path[PATH_MAX];

        if (mapped_file((const void *) &code_owners_marker, path, sizeof(path))) {
            add_path_owners(path);
        }
    }
#elif defined(HAVE_DLFCN_H) && defined(HAVE_DLADDR)
    {
        Dl_info info;
        char real[PATH_MAX];

        if (0 != dladdr((const void *) &code_owners_marker, &info) &&
            NULL != info.dli_fname && '/' == info.dli_fname[0]) {
            add_path_owners(info.dli_fname);
            if (NULL != realpath(info.dli_fname, real)) {
                add_path_owners(real);
            }
        }
    }
#endif
}

static bool code_owner(uid_t uid)
{
    int i;

    if (!code_owners_found) {
        find_code_owners();
    }
    for (i = 0; i < ncode_owners; i++) {
        if (code_owners[i] == uid) {
            return true;
        }
    }
    return false;
}

/* NULL if only root, we, those who could have changed the code we run (see
 * code_owner()) and - unless we are root - our groups could have written
 * what st describes, the file or (directory) the directory holding it;
 * else what is wrong. */
static const char *writers_unsound(const struct stat *st, bool directory)
{
    if (0 != st->st_uid && geteuid() != st->st_uid && !code_owner(st->st_uid)) {
        return directory ? "its directory is owned by a user who owns neither the library "
                           "we run nor a directory above it"
                         : "it is owned by a user who owns neither the library we run nor "
                           "a directory above it";
    }
    if (directory && 0 != (st->st_mode & S_ISVTX)) {
        return NULL;
    }
    if (0 != (st->st_mode & S_IWOTH)) {
        return directory ? "its directory can be written by anyone"
                         : "it can be written by anyone";
    }
    if (0 != (st->st_mode & S_IWGRP) && (0 == geteuid() || !our_group(st->st_gid))) {
        if (0 == geteuid()) {
            return directory ? "its directory can be written by its group, and we run as root"
                             : "it can be written by its group, and we run as root";
        }
        return directory ? "its directory can be written by a group we are not in"
                         : "it can be written by a group we are not in";
    }
    return NULL;
}

static const char *directory_unsound(const char *path)
{
    struct stat st;
    char *copy, *dir;
    const char *why;

    copy = strdup(path);
    if (NULL == copy) {
        return "its directory could not be checked";
    }
    dir = dirname(copy);
    why = (0 != stat(dir, &st)) ? "its directory could not be checked"
                                : writers_unsound(&st, true);
    free(copy);
    return why;
}

/* NULL if the file open on fd, at path, may be read */
static const char *paramfile_unsound(int fd, const char *path)
{
    struct stat st;
    char real[PATH_MAX];
    const char *why;

    if (0 != fstat(fd, &st)) {
        return "it could not be checked";
    }
    if (!S_ISREG(st.st_mode)) {
        return "it is not a regular file";
    }
    if (NULL != (why = writers_unsound(&st, false))) {
        return why;
    }
    if (NULL != (why = directory_unsound(path))) {
        return why;
    }
    /* reached through a link: the directory of what it names, too */
    if (0 == lstat(path, &st) && S_ISLNK(st.st_mode)) {
        if (NULL == realpath(path, real)) {
            return "the file it links to could not be checked";
        }
        if (NULL != (why = directory_unsound(real))) {
            return why;
        }
    }
    return NULL;
}

int pmix_util_keyval_parse(const char *filename, pmix_keyval_parse_fn_t callback,
                           void *cbdata)
{
    FILE *fp;
    int fd, flags;
    const char *why;
    int ret = PMIX_SUCCESS;

    pmix_mutex_lock(&keyval_mutex);

    /* O_NONBLOCK so that a FIFO at the name cannot hold up the open,
     * O_NOCTTY so that a terminal there does not become our controlling
     * terminal; a regular file is then read with blocking set again */
    fd = open(filename, O_RDONLY | O_NONBLOCK | O_NOCTTY | PMIX_O_CLOEXEC);
    if (0 > fd) {
        /* Our caller treats PMIX_ERR_NOT_FOUND as "there is no such file,
         * carry on", which is right for the default parameter files since
         * most systems have none of them.  It is not right for a file that
         * is there and could not be read - a permission problem or an
         * exhausted descriptor table discards every parameter in it and
         * looks exactly like the file never existing.  Keep the return
         * code, since failing startup over an unreadable optional dotfile
         * would be worse, but do not let it pass without a word. */
        if (ENOENT != errno) {
            pmix_output(0, "keyval parser: cannot read file %s: %s", filename,
                        strerror(errno));
        }
        ret = PMIX_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* A file somebody else could have written is passed over, as a file
     * that is not there would be, but said so - see writers_unsound() */
    if (NULL != (why = paramfile_unsound(fd, filename))) {
        pmix_output(0, "keyval parser: ignoring file %s: %s", filename, why);
        close(fd);
        ret = PMIX_ERR_NOT_FOUND;
        goto cleanup;
    }
    flags = fcntl(fd, F_GETFL);
    if (0 > flags || 0 > fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) ||
        NULL == (fp = fdopen(fd, "r"))) {
        pmix_output(0, "keyval parser: cannot read file %s: %s", filename, strerror(errno));
        close(fd);
        ret = PMIX_ERR_NOT_FOUND;
        goto cleanup;
    }

    ret = parse_stream(fp, filename, 0, callback, cbdata);
    fclose(fp);

cleanup:
    pmix_mutex_unlock(&keyval_mutex);

    return ret;
}

int pmix_util_keyval_save_internal_envars(pmix_keyval_parse_fn_t callback,
                                          void *cbdata)
{
    /* env_str is accumulated by the parser under this same lock, so
     * take it here as well - otherwise we can hand out (and free) the
     * string while another thread's parse is still appending to it */
    pmix_mutex_lock(&keyval_mutex);

    if (NULL != env_str && 0 < envlen) {
        callback(NULL, 0, "mca_base_env_list_internal", env_str, cbdata);
        free(env_str);
        env_str = NULL;
        envlen = 0;
    }

    pmix_mutex_unlock(&keyval_mutex);
    return PMIX_SUCCESS;
}
