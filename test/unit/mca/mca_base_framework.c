/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the framework lifecycle in
 * src/mca/base/pmix_mca_base_framework.c: register / open / close, the
 * reference count that ties them together, the REGISTERED and OPEN
 * state bits, the MCA variables a registration creates, and the
 * NOREGISTER and NO_DSO flags.
 *
 * The subject is a framework declared right here with no components at
 * all. That is deliberate: driving one of the library's real frameworks
 * would drag in whatever its components do, and the behavior under test
 * belongs entirely to the base.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/runtime/pmix_init_util.h"

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

/* ------------------------------------------------------------------ */
/* the framework under test                                            */
/* ------------------------------------------------------------------ */

static int n_register_calls = 0;
static int n_open_calls = 0;
static int n_close_calls = 0;
static int register_result = PMIX_SUCCESS;
static int open_result = PMIX_SUCCESS;

static int utestfw_register(pmix_mca_base_register_flag_t flags)
{
    (void) flags;
    n_register_calls++;
    return register_result;
}

static int utestfw_open(pmix_mca_base_open_flag_t flags)
{
    (void) flags;
    n_open_calls++;
    return open_result;
}

static int utestfw_close(void)
{
    n_close_calls++;
    return PMIX_SUCCESS;
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, utestfw, "a framework with no components",
                                utestfw_register, utestfw_open, utestfw_close,
                                NULL, PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

/* a second one that opts out of the MCA variable stage entirely */
PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, utestnoreg, "a NOREGISTER framework",
                                NULL, NULL, NULL, NULL,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER
                                    | PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);

/* A component stanza that states its framework's interface version as
 * three literal numbers instead of reaching for the ones the framework's
 * header states. No real component does this - they all open with
 * PMIX_MCA_BASE_VERSION(<framework>) - but these tests need components
 * whose version deliberately is *not* the framework's, which that macro
 * cannot express by construction. */
#define UTEST_COMPONENT_VERSION(type, maj, min, rel)                          \
    PMIX_MCA_BASE_VERSION_2_1_0("pmix", PMIX_MAJOR_VERSION,                   \
                                PMIX_MINOR_VERSION, PMIX_RELEASE_VERSION,     \
                                type, maj, min, rel)

/* A third framework, this one with a real static component, so that a
 * registration can fail *after* component discovery has already put
 * something on framework_components. See
 * test_failed_register_unwinds_components(). */
static pmix_mca_base_component_t utestreg_alpha = {
    UTEST_COMPONENT_VERSION("utestreg", 1, 0, 0),
    .pmix_mca_component_name = "alpha",
    .pmix_mca_component_major_version = 1,
    .pmix_mca_component_minor_version = 0,
    .pmix_mca_component_release_version = 0,
};

static const pmix_mca_base_component_t *utestreg_components[] = {&utestreg_alpha, NULL};
static const pmix_mca_base_component_t **utestreg_static_components[] = {utestreg_components,
                                                                         NULL};

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, utestreg, "a framework with one static component",
                                NULL, NULL, NULL,
                                (const pmix_mca_base_component_t ***) utestreg_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);

/* A fourth framework, this one stating an interface version, with two
 * static components: one built against that version and one built
 * against the previous one. See test_version_check(). The three
 * PMIX_MCA_utestver_*_VERSION macros are what a real framework puts in
 * its <framework>.h, and are the only place the number appears - the
 * declaration below reaches them by pasting the framework's name, and
 * so does the component stanza. */
#define PMIX_MCA_utestver_MAJOR_VERSION   2
#define PMIX_MCA_utestver_MINOR_VERSION   0
#define PMIX_MCA_utestver_RELEASE_VERSION 0

static pmix_mca_base_component_t utestver_current = {
    PMIX_MCA_BASE_VERSION(utestver),
    .pmix_mca_component_name = "current",
    .pmix_mca_component_major_version = 1,
    .pmix_mca_component_minor_version = 0,
    .pmix_mca_component_release_version = 0,
};

/* what an installed plugin from before the bump presents */
static pmix_mca_base_component_t utestver_stale = {
    UTEST_COMPONENT_VERSION("utestver", 1, 0, 0),
    .pmix_mca_component_name = "stale",
    .pmix_mca_component_major_version = 1,
    .pmix_mca_component_minor_version = 0,
    .pmix_mca_component_release_version = 0,
};

/* The shape configure generates into a framework's static-components.h:
 * an array of pointers *to* component pointers, one entry per
 * component. pmix_mca_base_component_find() reads one component out of
 * each entry, so a list of components hung off a single entry would
 * present only its first. */
static const pmix_mca_base_component_t *utestver_current_ptr = &utestver_current;
static const pmix_mca_base_component_t *utestver_stale_ptr = &utestver_stale;
static const pmix_mca_base_component_t **utestver_static_components[] = {&utestver_current_ptr,
                                                                        &utestver_stale_ptr, NULL};

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, utestver, "a framework stating a version",
                                          NULL, NULL, NULL,
                                          (const pmix_mca_base_component_t ***)
                                              utestver_static_components,
                                          PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);

/* And a fifth declared the unversioned way - the shape every framework
 * outside this project has, since PMIX_MCA_BASE_FRAMEWORK_DECLARE is
 * reached through an installed header and PRRTE declares all of its
 * frameworks with it. Its components state versions that match nothing;
 * they must all survive, because a framework that states no version of
 * its own has nothing to compare them against. */
static pmix_mca_base_component_t utestnover_alpha = {
    UTEST_COMPONENT_VERSION("utestnover", 1, 0, 0),
    .pmix_mca_component_name = "alpha",
    .pmix_mca_component_major_version = 1,
    .pmix_mca_component_minor_version = 0,
    .pmix_mca_component_release_version = 0,
};

static pmix_mca_base_component_t utestnover_beta = {
    UTEST_COMPONENT_VERSION("utestnover", 7, 3, 0),
    .pmix_mca_component_name = "beta",
    .pmix_mca_component_major_version = 1,
    .pmix_mca_component_minor_version = 0,
    .pmix_mca_component_release_version = 0,
};

static const pmix_mca_base_component_t *utestnover_alpha_ptr = &utestnover_alpha;
static const pmix_mca_base_component_t *utestnover_beta_ptr = &utestnover_beta;
static const pmix_mca_base_component_t **utestnover_static_components[] = {&utestnover_alpha_ptr,
                                                                          &utestnover_beta_ptr,
                                                                          NULL};

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, utestnover, "an unversioned framework", NULL, NULL, NULL,
                                (const pmix_mca_base_component_t ***) utestnover_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);

static void reset_counters(void)
{
    n_register_calls = 0;
    n_open_calls = 0;
    n_close_calls = 0;
    register_result = PMIX_SUCCESS;
    open_result = PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* register / open / close and the reference count                     */
/* ------------------------------------------------------------------ */

static void test_register_open_close(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestfw_base_framework;
    int rc;

    reset_counters();

    report("starts unregistered", !pmix_mca_base_framework_is_registered(fw));
    report("starts closed", !pmix_mca_base_framework_is_open(fw));
    report("starts with refcnt 0", 0 == fw->framework_refcnt);

    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("register succeeds", PMIX_SUCCESS == rc);
    report("register called the framework's register function", 1 == n_register_calls);
    report("register sets REGISTERED", pmix_mca_base_framework_is_registered(fw));
    report("register does not set OPEN", !pmix_mca_base_framework_is_open(fw));
    report("register takes a reference", 1 == fw->framework_refcnt);

    /* registering creates the framework's selection variable and its
     * verbosity variable */
    report("register created the selection variable",
           0 <= pmix_mca_base_var_find("pmix", "utestfw", NULL, NULL));
    report("register created the verbosity variable",
           0 <= pmix_mca_base_var_find("pmix", "utestfw", "base", "verbose"));

    /* a second register is just a reference */
    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("re-register succeeds", PMIX_SUCCESS == rc);
    report("re-register does not call the register function again", 1 == n_register_calls);
    report("re-register takes another reference", 2 == fw->framework_refcnt);

    rc = pmix_mca_base_framework_close(fw);
    report("close drops the extra reference", PMIX_SUCCESS == rc && 1 == fw->framework_refcnt);
    report("framework still registered after the extra reference went",
           pmix_mca_base_framework_is_registered(fw));

    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("open succeeds", PMIX_SUCCESS == rc);
    report("open called the framework's open function", 1 == n_open_calls);
    report("open sets OPEN", pmix_mca_base_framework_is_open(fw));
    report("open registered again, so it took a reference", 2 == fw->framework_refcnt);

    rc = pmix_mca_base_framework_close(fw);
    report("first close does not tear down", PMIX_SUCCESS == rc && 0 == n_close_calls);
    report("first close leaves it open", pmix_mca_base_framework_is_open(fw));
    report("first close drops one reference", 1 == fw->framework_refcnt);

    rc = pmix_mca_base_framework_close(fw);
    report("last close succeeds", PMIX_SUCCESS == rc);
    report("last close called the framework's close function", 1 == n_close_calls);
    report("last close clears OPEN", !pmix_mca_base_framework_is_open(fw));
    report("last close clears REGISTERED", !pmix_mca_base_framework_is_registered(fw));
    report("last close returns to refcnt 0", 0 == fw->framework_refcnt);

    /* closing an already-closed framework is a no-op, not an underflow */
    rc = pmix_mca_base_framework_close(fw);
    report("close on a closed framework is a no-op",
           PMIX_SUCCESS == rc && 0 == fw->framework_refcnt && 1 == n_close_calls);
}

/* A failed registration must put the reference count back. If it does
 * not, the framework can never reach zero again: the next close drops
 * the leaked reference instead of tearing down, so the close function
 * never runs and every component the framework holds is leaked. */
static void test_failed_register_restores_refcount(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestfw_base_framework;
    int rc;

    reset_counters();
    register_result = PMIX_ERR_BAD_PARAM;

    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("failed register reports the failure", PMIX_SUCCESS != rc);
    report("failed register did call the register function", 1 == n_register_calls);
    report("failed register leaves REGISTERED clear",
           !pmix_mca_base_framework_is_registered(fw));
    report("failed register restores refcnt to 0", 0 == fw->framework_refcnt);

    /* and the framework must still be usable afterwards */
    reset_counters();
    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("register after a failed register succeeds", PMIX_SUCCESS == rc);
    report("register after a failed register takes exactly one reference",
           1 == fw->framework_refcnt);

    rc = pmix_mca_base_framework_close(fw);
    report("close after the recovery tears down", PMIX_SUCCESS == rc);
    report("close after the recovery returns to refcnt 0", 0 == fw->framework_refcnt);
    report("close after the recovery clears REGISTERED",
           !pmix_mca_base_framework_is_registered(fw));
}

/* A failed registration must also give back everything it built, not
 * only the reference count.
 *
 * This is the case a bare "refcnt--" misses. Component discovery runs
 * inside register(), appending every component that matched the
 * selection, and component_find_check() only afterwards rejects the run
 * over a requested name that matched nothing -- so by the time the
 * failure surfaces, framework_components is already populated. Nothing
 * else will ever drain it: pmix_mca_base_framework_close() returns
 * immediately when neither REGISTERED nor OPEN is set, which is exactly
 * the state a failed register leaves behind.
 *
 * main() arranges the failure with mca_base_abort_on_load_error and a
 * selection naming this framework's one real component plus one that
 * does not exist -- ordinary user input, no allocation failure. */
static void test_failed_register_unwinds_components(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestreg_base_framework;
    int rc;

    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("register fails on a selection naming a missing component",
           PMIX_ERR_NOT_FOUND == rc);
    report("the failed register restores refcnt to 0", 0 == fw->framework_refcnt);
    report("the failed register leaves REGISTERED clear",
           !pmix_mca_base_framework_is_registered(fw));
    report("the failed register drained the components it had found",
           0 == pmix_list_get_size(&fw->framework_components));
    report("the failed register drained the failed-component list",
           0 == pmix_list_get_size(&fw->framework_failed_components));

    /* and close on the wreckage is still a clean no-op */
    rc = pmix_mca_base_framework_close(fw);
    report("close after a failed register is a no-op",
           PMIX_SUCCESS == rc && 0 == fw->framework_refcnt);
}

/* A failed open has to undo the registration it performed on the way in.
 *
 * Merely decrementing is not enough. pmix_mca_base_framework_open()
 * registers first, so when the caller is the first one in, the failure
 * path has to put REGISTERED back as well -- otherwise the framework
 * reports itself registered with a reference count of zero, and the
 * next close() decrements 0 to -1, finds that non-zero, and returns
 * success without tearing anything down. The framework is then wedged:
 * no later close can ever reach zero. */
static void test_failed_open_when_sole_holder(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestfw_base_framework;
    int rc;

    reset_counters();
    open_result = PMIX_ERR_NOT_SUPPORTED;

    report("precondition: framework starts idle",
           0 == fw->framework_refcnt && !pmix_mca_base_framework_is_registered(fw));

    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("failed open reports the failure", PMIX_SUCCESS != rc);
    report("failed open leaves OPEN clear", !pmix_mca_base_framework_is_open(fw));
    report("failed open restores refcnt to 0", 0 == fw->framework_refcnt);
    report("failed open also clears REGISTERED it had just set",
           !pmix_mca_base_framework_is_registered(fw));
    report("failed open did not run the close function", 0 == n_close_calls);

    /* the framework must be usable afterwards -- in particular the next
     * close must be able to reach zero */
    reset_counters();
    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("open after a failed open succeeds", PMIX_SUCCESS == rc);
    report("open after a failed open takes exactly one reference",
           1 == fw->framework_refcnt);
    rc = pmix_mca_base_framework_close(fw);
    report("close after the recovery tears down",
           PMIX_SUCCESS == rc && 1 == n_close_calls);
    report("close after the recovery returns to refcnt 0", 0 == fw->framework_refcnt);
}

/* The other half of that contract: when somebody else already holds a
 * reference, a failed open must give back only its own -- it must not
 * tear down a framework that is still in use. */
static void test_failed_open_when_not_sole_holder(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestfw_base_framework;
    int rc;

    reset_counters();

    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("holder registers the framework",
           PMIX_SUCCESS == rc && 1 == fw->framework_refcnt);

    open_result = PMIX_ERR_NOT_SUPPORTED;
    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("failed open still reports the failure", PMIX_SUCCESS != rc);
    report("failed open gave back only its own reference", 1 == fw->framework_refcnt);
    report("failed open left the other holder's registration intact",
           pmix_mca_base_framework_is_registered(fw));
    report("failed open did not run the close function", 0 == n_close_calls);

    /* and the holder's own close still tears down */
    rc = pmix_mca_base_framework_close(fw);
    report("the holder's close tears down",
           PMIX_SUCCESS == rc && 0 == fw->framework_refcnt
               && !pmix_mca_base_framework_is_registered(fw));
}

/* Repeated open/close cycles have to be idempotent: the variables are
 * deregistered and re-registered, the component lists are destroyed and
 * reconstructed, and the state bits have to come back to where they
 * started every time. */
static void test_cycling(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestfw_base_framework;
    int i, rc;
    bool ok = true;

    reset_counters();

    for (i = 0; i < 5; ++i) {
        rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
        if (PMIX_SUCCESS != rc || !pmix_mca_base_framework_is_open(fw)) {
            ok = false;
            break;
        }
        /* while the framework is open its variables are registered */
        if (0 > pmix_mca_base_var_find("pmix", "utestfw", NULL, NULL)
            || 0 > pmix_mca_base_var_find("pmix", "utestfw", "base", "verbose")) {
            ok = false;
            break;
        }
        rc = pmix_mca_base_framework_close(fw);
        if (PMIX_SUCCESS != rc || pmix_mca_base_framework_is_open(fw)
            || 0 != fw->framework_refcnt) {
            ok = false;
            break;
        }
        /* and closing deregisters the whole variable group with it --
         * that is the point of grouping them by owner */
        if (0 <= pmix_mca_base_var_find("pmix", "utestfw", NULL, NULL)) {
            ok = false;
            break;
        }
    }
    report("five open/close cycles leave the framework where it started", ok);
    report("each cycle ran the open function", 5 == n_open_calls);
    report("each cycle ran the close function", 5 == n_close_calls);

    /* one more open, to show the deregistered variables come back rather
     * than the group staying dead after the first cycle */
    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("the variable group is revived by a later open",
           PMIX_SUCCESS == rc && 0 <= pmix_mca_base_var_find("pmix", "utestfw", NULL, NULL));
    (void) pmix_mca_base_framework_close(fw);
    report("framework left idle for the next test",
           0 == fw->framework_refcnt && !pmix_mca_base_framework_is_registered(fw));
}

/* A NOREGISTER framework skips the MCA variable stage entirely, but
 * still opens and closes. */
static void test_noregister(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestnoreg_base_framework;
    int rc;

    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("NOREGISTER framework opens", PMIX_SUCCESS == rc);
    report("NOREGISTER framework reports open", pmix_mca_base_framework_is_open(fw));
    report("NOREGISTER framework registered no selection variable",
           0 > pmix_mca_base_var_find("pmix", "utestnoreg", NULL, NULL));
    report("NOREGISTER framework registered no verbosity variable",
           0 > pmix_mca_base_var_find("pmix", "utestnoreg", "base", "verbose"));

    rc = pmix_mca_base_framework_close(fw);
    report("NOREGISTER framework closes", PMIX_SUCCESS == rc);
    report("NOREGISTER framework returns to refcnt 0", 0 == fw->framework_refcnt);
}

/* The framework interface version: where it comes from, and what
 * open_components() does with it.
 *
 * A component is a run-time-loadable plugin, so an installed one can be
 * older than the library that loads it, and its module struct is
 * whatever its header said at the time. Nothing else in the MCA compares
 * the two -- the repository's own check is on the *MCA* version, which
 * is one number for the whole project.
 *
 * Two things are pinned here. First, that the versioned declaration
 * really does pick the numbers up from the framework's own
 * PMIX_MCA_<name>_*_VERSION macros: that is what keeps a bump to one
 * edit in one header rather than a number restated at the declaration
 * and left behind. Second, that an unversioned framework is exempt
 * rather than emptied. */
static void test_version_check(void)
{
    pmix_mca_base_framework_t *fw = &pmix_utestver_base_framework;
    pmix_mca_base_component_list_item_t *cli;
    int rc;

    report("the versioned declaration took the major from the header macro",
           2 == fw->framework_type_major_version);
    report("the versioned declaration took the minor from the header macro",
           0 == fw->framework_type_minor_version);
    report("an unversioned framework reports no version",
           0 == pmix_utestfw_base_framework.framework_type_major_version
               && 0 == pmix_utestfw_base_framework.framework_type_minor_version);

    /* Register first, and check the discovery result before opening.
     * Discovery does not look at versions, so both components must be on
     * the list at this point - otherwise "one survived the open" would
     * pass whether or not the check ever ran. */
    rc = pmix_mca_base_framework_register(fw, PMIX_MCA_BASE_REGISTER_DEFAULT);
    report("the versioned framework registers", PMIX_SUCCESS == rc);
    report("discovery finds both components, versions notwithstanding",
           2 == pmix_list_get_size(&fw->framework_components));

    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("the versioned framework opens", PMIX_SUCCESS == rc);
    report("opening refused the stale component",
           1 == pmix_list_get_size(&fw->framework_components));
    cli = (pmix_mca_base_component_list_item_t *) pmix_list_get_first(&fw->framework_components);
    report("the component built against the current version survived",
           NULL != cli && 0 == strcmp("current", cli->cli_component->pmix_mca_component_name));

    rc = pmix_mca_base_framework_close(fw);
    report("the versioned framework gives back the open reference",
           PMIX_SUCCESS == rc && 1 == fw->framework_refcnt);
    rc = pmix_mca_base_framework_close(fw);
    report("the versioned framework closes", PMIX_SUCCESS == rc && 0 == fw->framework_refcnt);

    /* the PRRTE-shaped case: no version stated, so nothing to refuse */
    fw = &pmix_utestnover_base_framework;
    rc = pmix_mca_base_framework_open(fw, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("the unversioned framework opens", PMIX_SUCCESS == rc);
    report("an unversioned framework keeps every component whatever it claims",
           2 == pmix_list_get_size(&fw->framework_components));

    rc = pmix_mca_base_framework_close(fw);
    report("the unversioned framework closes", PMIX_SUCCESS == rc && 0 == fw->framework_refcnt);
}

int main(int argc, char **argv)
{
    int rc;

    (void) argc;
    (void) argv;

    /* Keep the caller's MCA parameter files out of the results, exactly
     * as mca_base_var.c does. */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    /* make component_find_check() reject a selection that names a
     * component this framework does not have -- see
     * test_failed_register_unwinds_components() */
    setenv("PMIX_MCA_mca_base_abort_on_load_error", "1", 1);
    setenv("PMIX_MCA_utestreg", "alpha,nosuchcomponent", 1);

    /* pmix_init_util() establishes the install dirs, the variable
     * system and the component repository. The last of those matters
     * here: registering a framework that permits DSO components reaches
     * pmix_mca_base_component_repository_get_components(), which reads a
     * hash table that only pmix_mca_base_open() constructs. */
    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== pmix_mca_base_framework unit tests ===\n\n");

    test_register_open_close();
    test_failed_register_restores_refcount();
    test_failed_register_unwinds_components();
    test_failed_open_when_sole_holder();
    test_failed_open_when_not_sole_holder();
    test_cycling();
    test_noregister();
    test_version_check();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    pmix_finalize_util();

    return (nfail > 0) ? 1 : 0;
}
