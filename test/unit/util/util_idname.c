/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the user- and group-ID conversions in
 * src/util/pmix_idname.c.
 *
 * PMIX_USERID and PMIX_GRPID may be given as a number or as a name. These
 * pin down what each form turns into: a string of decimal digits is the
 * number, any other string is looked up by name, and anything that
 * cannot be resolved is an error rather than some default. This
 * process's own user and group are the names the cases look up, since
 * they are the only ones every machine is sure to have.
 *
 * Exit 0 if all tests pass, 1 otherwise, 77 to skip.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_idname.h"

#define IDN_UNKNOWN "pmix-idname-ut-no-such-name"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

static void test_strings(const char *myuser, const char *mygroup)
{
    uint32_t id = 0;

    report("a decimal string is a uid",
           PMIX_SUCCESS == pmix_util_uid_from_string("4242", &id) && 4242 == id);
    report("a decimal string is a gid",
           PMIX_SUCCESS == pmix_util_gid_from_string("4343", &id) && 4343 == id);
    report("a number too big for an ID is refused",
           PMIX_ERR_BAD_PARAM == pmix_util_uid_from_string("99999999999", &id));
    report("an empty string is refused", PMIX_ERR_BAD_PARAM == pmix_util_gid_from_string("", &id));
    report("a NULL string is refused", PMIX_ERR_BAD_PARAM == pmix_util_uid_from_string(NULL, &id));
    report("an unknown user name is not found",
           PMIX_ERR_NOT_FOUND == pmix_util_uid_from_string(IDN_UNKNOWN, &id));
    report("an unknown group name is not found",
           PMIX_ERR_NOT_FOUND == pmix_util_gid_from_string(IDN_UNKNOWN, &id));
    report("a string that is not all digits is a name, not a number",
           PMIX_ERR_NOT_FOUND == pmix_util_gid_from_string("12abc", &id));
    if (NULL != myuser) {
        id = 0;
        report("our own user name is our uid",
               PMIX_SUCCESS == pmix_util_uid_from_string(myuser, &id) && (uint32_t) geteuid() == id);
    } else {
        fprintf(stdout, "  SKIP: this process's user has no name\n");
    }
    if (NULL != mygroup) {
        id = 0;
        report("our own group name is our gid",
               PMIX_SUCCESS == pmix_util_gid_from_string(mygroup, &id) && (uint32_t) getegid() == id);
    } else {
        fprintf(stdout, "  SKIP: this process's group has no name\n");
    }
}

static void test_values(const char *mygroup)
{
    pmix_value_t val;
    uint32_t id = 0, u32 = 5151;
    int neg = -1;
    bool flag = true;

    PMIX_VALUE_LOAD(&val, &u32, PMIX_UINT32);
    report("a uint32 value is taken as it is",
           PMIX_SUCCESS == pmix_util_uid_from_value(&val, &id) && 5151 == id);
    PMIX_VALUE_DESTRUCT(&val);

    PMIX_VALUE_LOAD(&val, &neg, PMIX_INT);
    report("a negative number is refused", PMIX_SUCCESS != pmix_util_gid_from_value(&val, &id));
    PMIX_VALUE_DESTRUCT(&val);

    PMIX_VALUE_LOAD(&val, &flag, PMIX_BOOL);
    report("a value of another type is refused",
           PMIX_ERR_BAD_PARAM == pmix_util_uid_from_value(&val, &id));
    PMIX_VALUE_DESTRUCT(&val);

    PMIX_VALUE_LOAD(&val, IDN_UNKNOWN, PMIX_STRING);
    report("a string value naming nobody is not found",
           PMIX_ERR_NOT_FOUND == pmix_util_gid_from_value(&val, &id));
    PMIX_VALUE_DESTRUCT(&val);

    if (NULL != mygroup) {
        PMIX_VALUE_LOAD(&val, mygroup, PMIX_STRING);
        id = 0;
        report("a string value naming our group is our gid",
               PMIX_SUCCESS == pmix_util_gid_from_value(&val, &id) && (uint32_t) getegid() == id);
        PMIX_VALUE_DESTRUCT(&val);
    }
    report("a NULL value is refused", PMIX_ERR_BAD_PARAM == pmix_util_uid_from_value(NULL, &id));
}

int main(int argc, char **argv)
{
    struct passwd *pw;
    struct group *gr;
    char *myuser = NULL, *mygroup = NULL;

    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== pmix_idname unit tests ===\n\n");

    pw = getpwuid(geteuid());
    if (NULL != pw && NULL != pw->pw_name) {
        myuser = strdup(pw->pw_name);
    }
    gr = getgrgid(getegid());
    if (NULL != gr && NULL != gr->gr_name) {
        mygroup = strdup(gr->gr_name);
    }

    test_strings(myuser, mygroup);
    test_values(mygroup);

    free(myuser);
    free(mygroup);
    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
