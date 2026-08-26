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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Buffer safe printf functions for portability to archaic platforms.
 */

#include "src/include/pmix_config.h"

#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_VASPRINTF

#if !HAVE_VSNPRINTF
/*
 * Upper bounds on what a single conversion can emit.  Over-estimating
 * costs a few unused bytes; under-estimating overruns the buffer that
 * vsprintf() is about to fill without a bound, so each of these is the
 * worst case rounded up.
 */
/* a 64-bit value in octal is 22 digits; leave room for a sign and a
 * "0x" or "0" prefix */
#define PMIX_GUESS_INT_CHARS 32
/* %f of DBL_MAX is 309 integral digits, plus a sign, the radix
 * character and the default precision of six */
#define PMIX_GUESS_DBL_CHARS 512
/* %Lf of LDBL_MAX reaches 4932 integral digits on x87 */
#define PMIX_GUESS_LDBL_CHARS 5120
/* a single character can expand to several bytes in a multibyte
 * locale */
#define PMIX_GUESS_CHAR_CHARS 16
/* "0x" and 16 hex digits, with slack for stranger renderings */
#define PMIX_GUESS_PTR_CHARS 32
#endif /* !HAVE_VSNPRINTF */

/*
 * Make a good guess about how long a printf-style varargs formatted
 * string will be once all the % escapes are filled in, and consume the
 * arguments it names from ap.
 *
 * Returns the number of bytes to allocate, or -1 if the format contains
 * something whose output cannot be bounded - an unrecognized conversion,
 * or a wide string with no precision.  The caller must fail on -1: a
 * conversion we cannot size is also a conversion whose argument we
 * cannot consume, so everything after it in the va_list would be read
 * as the wrong type.
 */
static int guess_strlen(const char *fmt, va_list ap)
{
#if HAVE_VSNPRINTF
    char dummy[1];
    int len;

    /* vsnprintf() returns the number of bytes that would have been
     copied if the provided buffer were infinite. */
    len = vsnprintf(dummy, sizeof(dummy), fmt, ap);
    if (0 > len || INT_MAX == len) {
        return -1;
    }
    return 1 + len;
#else
    enum {
        GUESS_LEN_INT,
        GUESS_LEN_LONG,
        GUESS_LEN_LLONG,
        GUESS_LEN_INTMAX,
        GUESS_LEN_SIZE,
        GUESS_LEN_PTRDIFF,
        GUESS_LEN_LDBL
    } lenmod;
    const char *sarg;
    size_t fmtlen;
    size_t total;
    size_t width;
    size_t prec;
    size_t chars;
    bool have_prec;
    size_t i;
    int iarg;

    fmtlen = strlen(fmt);

    /* Start off with a fudge factor of 128 to cover whatever the walk
     below gets wrong */
    total = fmtlen + 128;

    for (i = 0; i < fmtlen; ++i) {
        if ('%' != fmt[i]) {
            continue;
        }
        ++i;
        if (i >= fmtlen || '%' == fmt[i]) {
            /* a trailing '%', or the "%%" escape: neither takes an
             argument */
            continue;
        }

        /* flags */
        while (i < fmtlen && NULL != strchr("-+ #0'", fmt[i])) {
            ++i;
        }

        /* field width */
        width = 0;
        if (i < fmtlen && '*' == fmt[i]) {
            iarg = va_arg(ap, int);
            width = (0 > iarg) ? (size_t)(-(long long) iarg) : (size_t) iarg;
            ++i;
        } else {
            while (i < fmtlen && isdigit((unsigned char) fmt[i])) {
                if ((size_t) INT_MAX >= width) {
                    width = (width * 10) + (size_t)(fmt[i] - '0');
                }
                ++i;
            }
        }

        /* precision */
        prec = 0;
        have_prec = false;
        if (i < fmtlen && '.' == fmt[i]) {
            ++i;
            have_prec = true;
            if (i < fmtlen && '*' == fmt[i]) {
                iarg = va_arg(ap, int);
                /* a negative precision is taken as if it were omitted */
                if (0 > iarg) {
                    have_prec = false;
                } else {
                    prec = (size_t) iarg;
                }
                ++i;
            } else {
                while (i < fmtlen && isdigit((unsigned char) fmt[i])) {
                    if ((size_t) INT_MAX >= prec) {
                        prec = (prec * 10) + (size_t)(fmt[i] - '0');
                    }
                    ++i;
                }
            }
        }

        /* length modifier */
        lenmod = GUESS_LEN_INT;
        if (i < fmtlen) {
            switch (fmt[i]) {
                case 'h':
                    /* both %hd and %hhd are promoted to int */
                    ++i;
                    if (i < fmtlen && 'h' == fmt[i]) {
                        ++i;
                    }
                    break;
                case 'l':
                    ++i;
                    if (i < fmtlen && 'l' == fmt[i]) {
                        ++i;
                        lenmod = GUESS_LEN_LLONG;
                    } else {
                        lenmod = GUESS_LEN_LONG;
                    }
                    break;
                case 'q':
                    ++i;
                    lenmod = GUESS_LEN_LLONG;
                    break;
                case 'j':
                    ++i;
                    lenmod = GUESS_LEN_INTMAX;
                    break;
                case 'z':
                    ++i;
                    lenmod = GUESS_LEN_SIZE;
                    break;
                case 't':
                    ++i;
                    lenmod = GUESS_LEN_PTRDIFF;
                    break;
                case 'L':
                    ++i;
                    lenmod = GUESS_LEN_LDBL;
                    break;
                default:
                    break;
            }
        }
        if (i >= fmtlen) {
            /* the format ended in the middle of a conversion */
            break;
        }

        /* the conversion itself.  Signed and unsigned types of the same
         width are interchangeable in va_arg, so one arm covers both. */
        chars = 0;
        switch (fmt[i]) {
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                switch (lenmod) {
                    case GUESS_LEN_LONG:
                        (void) va_arg(ap, long);
                        break;
                    case GUESS_LEN_LLONG:
                        (void) va_arg(ap, long long);
                        break;
                    case GUESS_LEN_INTMAX:
                        (void) va_arg(ap, intmax_t);
                        break;
                    case GUESS_LEN_SIZE:
                        (void) va_arg(ap, size_t);
                        break;
                    case GUESS_LEN_PTRDIFF:
                        (void) va_arg(ap, ptrdiff_t);
                        break;
                    default:
                        (void) va_arg(ap, int);
                        break;
                }
                /* a precision on an integer conversion is a minimum
                 number of digits */
                chars = PMIX_GUESS_INT_CHARS + prec;
                break;

            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                if (GUESS_LEN_LDBL == lenmod) {
                    (void) va_arg(ap, long double);
                    chars = PMIX_GUESS_LDBL_CHARS + prec;
                } else {
                    (void) va_arg(ap, double);
                    chars = PMIX_GUESS_DBL_CHARS + prec;
                }
                break;

            case 'c':
                /* %lc names a wint_t, which is an integer-promoted type
                 the same width as int everywhere we build; take it as an
                 int rather than requiring <wchar.h> here */
                (void) va_arg(ap, int);
                chars = PMIX_GUESS_CHAR_CHARS;
                break;

            case 's':
                sarg = va_arg(ap, const char *);
                if (GUESS_LEN_LONG == lenmod) {
                    /* a wchar_t string cannot be measured from here, so
                     an explicit precision is the only bound we have */
                    if (!have_prec) {
                        return -1;
                    }
                    chars = prec * PMIX_GUESS_CHAR_CHARS;
                } else if (NULL == sarg) {
#if PMIX_ENABLE_DEBUG
                    pmix_output(0,
                                "PMIX DEBUG WARNING: Got a NULL argument to pmix_vasprintf!\n");
#endif
                    /* implementations render this as "(null)" */
                    chars = 8;
                } else {
                    chars = strlen(sarg);
                    if (have_prec && prec < chars) {
                        chars = prec;
                    }
                }
                break;

            case 'p':
                (void) va_arg(ap, void *);
                chars = PMIX_GUESS_PTR_CHARS;
                break;

            case 'n':
                /* consumes a pointer and emits nothing */
                (void) va_arg(ap, void *);
                break;

            default:
                /* we do not know what this consumes, so we cannot keep
                 reading the va_list either */
                return -1;
        }

        if (chars < width) {
            chars = width;
        }
        if ((size_t) INT_MAX < chars || (size_t) INT_MAX - chars < total) {
            return -1;
        }
        total += chars;
    }

    return (int) total;
#endif
}

#endif /* #ifndef HAVE_VASPRINTF */

int pmix_asprintf(char **ptr, const char *fmt, ...)
{
    int length;
    va_list ap;

    va_start(ap, fmt);
    /* pmix_vasprintf guarantees that *ptr is set to NULL on error */
    length = pmix_vasprintf(ptr, fmt, ap);
    va_end(ap);

    return length;
}

int pmix_vasprintf(char **ptr, const char *fmt, va_list ap)
{
#ifdef HAVE_VASPRINTF
    int length;

    length = vasprintf(ptr, fmt, ap);
    if (0 > length) {
        *ptr = NULL;
    }

    return length;
#else
    char *buf;
    char *tmp;
    int length;
    va_list ap2;

    /* va_list might have pointer to internal state and using
     it twice is a bad idea.  So make a copy for the second
     use.  Copy order taken from Autoconf docs. */
#if PMIX_HAVE_VA_COPY
    va_copy(ap2, ap);
#elif PMIX_HAVE_UNDERSCORE_VA_COPY
    __va_copy(ap2, ap);
#else
    memcpy(&ap2, &ap, sizeof(va_list));
#endif

    /* guess the size */
    length = guess_strlen(fmt, ap);
    if (0 > length) {
        /* the format holds something we cannot bound; refuse it rather
         than hand vsprintf() a buffer it would run off the end of */
#if PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY
        va_end(ap2);
#endif /* PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY */
        *ptr = NULL;
        errno = EINVAL;
        return -1;
    }

    /* allocate a buffer */
    buf = (char *) calloc(((size_t) length + 1), sizeof(char));
    if (NULL == buf) {
#if PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY
        va_end(ap2);
#endif /* PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY */
        *ptr = NULL;
        errno = ENOMEM;
        return -1;
    }

    /* fill the buffer */
    length = vsprintf(buf, fmt, ap2);
#if PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY
    va_end(ap2);
#endif /* PMIX_HAVE_VA_COPY || PMIX_HAVE_UNDERSCORE_VA_COPY */
    if (0 > length) {
        free(buf);
        *ptr = NULL;
        return -1;
    }

    /* hand back the slack the guess left over.  Failing to shrink an
     allocation is not a reason to throw away a string we already
     formatted successfully - just keep the larger buffer. */
    tmp = (char *) realloc(buf, (size_t) length + 1);
    if (NULL != tmp) {
        buf = tmp;
    }

    *ptr = buf;
    return length;
#endif
}

int pmix_snprintf(char *str, size_t size, const char *fmt, ...)
{
    int length;
    va_list ap;

    va_start(ap, fmt);
    length = pmix_vsnprintf(str, size, fmt, ap);
    va_end(ap);

    return length;
}

int pmix_vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
#if HAVE_VSNPRINTF
    /* C99 vsnprintf() already has exactly these semantics, and reaches
     them without allocating.  Going the long way round through
     pmix_vasprintf() would put a malloc/free pair on every line the
     library prints, and would fail outright under the memory pressure
     that the diagnostics are usually there to report. */
    if (NULL == str || 0 == size) {
        /* the C99 "how long would it be" form; a NULL buffer with a
         non-zero size is undefined, so normalize it here */
        return vsnprintf(NULL, 0, fmt, ap);
    }
    return vsnprintf(str, size, fmt, ap);
#else
    int length;
    char *buf;

    length = pmix_vasprintf(&buf, fmt, ap);
    if (0 > length) {
        /* the header promises the output is always null-terminated,
         and a caller that ignores the return value will read this
         buffer regardless */
        if (NULL != str && 0 < size) {
            str[0] = '\0';
        }
        return length;
    }

    /* return the length when given a null buffer (C99) */
    if (NULL != str && 0 < size) {
        if ((size_t) length < size) {
            strcpy(str, buf);
        } else {
            memcpy(str, buf, size - 1);
            str[size - 1] = '\0';
        }
    }

    /* free allocated buffer */
    free(buf);

    return length;
#endif
}
