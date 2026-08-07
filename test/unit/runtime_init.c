/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the bring-up decisions made in src/runtime -
 * specifically the parts of pmix_rte_init/pmix_params that turn what the
 * host hands the library into process-wide state.
 *
 * What is covered:
 *
 *   help topics   Every show_help topic src/runtime references must
 *                 actually exist in help-pmix-runtime.txt. A missing one
 *                 is invisible until the rare path that emits it fires,
 *                 and then the user gets "I couldn't find that help
 *                 reference" instead of the diagnostic. Asking for the
 *                 topic is the cheap way to keep the two in step.
 *
 *   hostname      pmix_set_aliases applies the FQDN policy and records
 *                 the form it did not keep as an alias, so that
 *                 pmix_check_local matches our node under either name.
 *                 This must happen no matter where the name came from:
 *                 a host that supplies PMIX_HOSTNAME (every RM does)
 *                 gets the same treatment as a name read from the OS.
 *
 *   var_dump_color  the pmix_var_dump_color MCA parameter is turned into
 *                 ready-to-emit ANSI escapes, and a malformed or
 *                 unrecognized entry is reported and skipped rather than
 *                 taking the good entries down with it.
 *
 *   output        a verbosity channel opened by pmix_rte_init is given
 *                 back by pmix_rte_finalize, id and all.
 *
 *   iof flags     the IOF output-file/directory strings the directive
 *                 scan hands to pmix_globals are released with it.
 *
 * Everything except the help topics is then re-checked over a second
 * init/finalize cycle: this layer's standing requirement is that a
 * second PMIx_Init starts from a clean slate, and per-cycle state that
 * is rebuilt wrongly - or not rebuilt at all - shows up nowhere else.
 *
 * The process comes up as a PMIx server because that is the role that
 * passes host-supplied directives - notably PMIX_HOSTNAME - through
 * pmix_rte_init.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix_server.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_show_help.h"

/* the hostname we hand the library at init, and the two forms of it that
 * must both resolve to "this node" afterwards */
#define UT_FQDN  "utnode.unit-test.example.org"
#define UT_SHORT "utnode"

/* what we set the color parameter to before init: one good entry, one
 * entry naming a key that does not exist, one entry with no '=' at all,
 * and one more good entry after the damage */
#define UT_COLORS "name=34,bogus_key=1,value,valid_values=36"

/* an IOF output-file directive: never actually written to (no client
 * ever produces output here), it just has to be captured and released */
#define UT_IOF_FILE "ut-runtime-init-output"

/* a nodeid the library would never invent for itself */
#define UT_NODEID 4242

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* ------------------------------------------------------------------ */
/* show_help topics referenced from src/runtime                        */
/* ------------------------------------------------------------------ */

/* pmix_show_help_string returns NULL - and prints a "couldn't find that
 * help reference" complaint - when the topic is not in the file, so a
 * non-NULL return is exactly the assertion we want. It takes the same
 * arguments the real call site does, so a topic whose format string
 * stops matching its caller shows up here too. */
static void check_topic(const char *topic, char *rendered)
{
    char label[256];

    snprintf(label, sizeof(label), "help:%s", topic);
    report(label, NULL != rendered);
    if (NULL != rendered) {
        free(rendered);
    }
}

static void test_help_topics(void)
{
    char *s;

    check_topic("pmix_init:startup:internal-failure",
                pmix_show_help_string("help-pmix-runtime.txt",
                                      "pmix_init:startup:internal-failure", false,
                                      "some stage", PMIX_ERR_BAD_PARAM));

    check_topic("blocking-call-from-progress-thread",
                pmix_show_help_string("help-pmix-runtime.txt",
                                      "blocking-call-from-progress-thread", false,
                                      "PMIx_Get"));

    check_topic("var_dump_color:format-error",
                pmix_show_help_string("help-pmix-runtime.txt",
                                      "var_dump_color:format-error", false,
                                      "value"));

    check_topic("var_dump_color:unknown-key",
                pmix_show_help_string("help-pmix-runtime.txt",
                                      "var_dump_color:unknown-key", false,
                                      "bogus_key", "bogus_key=1",
                                      "name,value,valid_values"));

    check_topic("progress-thread:bad-cpu-list",
                pmix_show_help_string("help-pmix-runtime.txt",
                                      "progress-thread:bad-cpu-list", false,
                                      "3-x", "0,3-x", 1023));

    /* and prove the check above has teeth: a topic that really is
     * missing must come back NULL */
    s = pmix_show_help_string("help-pmix-runtime.txt",
                              "ut-no-such-topic-should-not-exist", false);
    report("help:missing topic reports missing", NULL == s);
    if (NULL != s) {
        free(s);
    }
}

/* ------------------------------------------------------------------ */
/* hostname / alias resolution                                         */
/* ------------------------------------------------------------------ */

static bool has_alias(char **aliases, const char *want)
{
    int n;

    if (NULL == aliases) {
        return false;
    }
    for (n = 0; NULL != aliases[n]; n++) {
        if (0 == strcmp(aliases[n], want)) {
            return true;
        }
    }
    return false;
}

/* The library was initialized with PMIX_HOSTNAME set to an FQDN and the
 * default FQDN policy (do not keep it). Both forms of the name must now
 * resolve to this node - which they only do if the aliases were built,
 * and they were not built for a host-supplied name before. */
static void test_supplied_hostname(void)
{
    report("hostname:short name kept",
           NULL != pmix_globals.hostname &&
           0 == strcmp(pmix_globals.hostname, UT_SHORT));

    report("hostname:fqdn retained as an alias",
           has_alias(pmix_globals.aliases, UT_FQDN));

    report("hostname:check_local matches the short name",
           pmix_check_local(UT_SHORT));

    report("hostname:check_local matches the fqdn",
           pmix_check_local(UT_FQDN));

    report("hostname:check_local rejects another node",
           !pmix_check_local("some-other-node.unit-test.example.org"));

    report("hostname:check_local tolerates NULL", !pmix_check_local(NULL));
}

/* pmix_set_aliases itself, driven directly over the cases that decide
 * which form of a name is kept and which becomes the alias. It rewrites
 * the name in place, so each case gets its own buffer. */
static void test_set_aliases(void)
{
    bool saved = pmix_keep_fqdn_hostnames;
    char **aliases;
    char *name;

    /* policy "drop the FQDN": the name is truncated to the short form
     * and the FQDN is kept as the alias */
    pmix_keep_fqdn_hostnames = false;
    aliases = NULL;
    name = strdup("node01.example.com");
    pmix_set_aliases(&aliases, name);
    report("aliases:drop-fqdn shortens the name", 0 == strcmp(name, "node01"));
    report("aliases:drop-fqdn keeps the fqdn as alias",
           has_alias(aliases, "node01.example.com"));
    PMIx_Argv_free(aliases);
    free(name);

    /* policy "keep the FQDN": the name is left alone and the short form
     * becomes the alias */
    pmix_keep_fqdn_hostnames = true;
    aliases = NULL;
    name = strdup("node01.example.com");
    pmix_set_aliases(&aliases, name);
    report("aliases:keep-fqdn leaves the name alone",
           0 == strcmp(name, "node01.example.com"));
    report("aliases:keep-fqdn keeps the short name as alias",
           has_alias(aliases, "node01"));
    PMIx_Argv_free(aliases);
    free(name);

    /* a name with no domain part has no second form - nothing to record */
    pmix_keep_fqdn_hostnames = false;
    aliases = NULL;
    name = strdup("node01");
    pmix_set_aliases(&aliases, name);
    report("aliases:no-domain leaves the name alone", 0 == strcmp(name, "node01"));
    report("aliases:no-domain adds no alias", NULL == aliases);
    free(name);

    /* an IP literal is full of dots but is not an FQDN - truncating it
     * would produce a different address, so it must be left intact */
    aliases = NULL;
    name = strdup("10.11.12.13");
    pmix_set_aliases(&aliases, name);
    report("aliases:ipv4 literal is left intact", 0 == strcmp(name, "10.11.12.13"));
    report("aliases:ipv4 literal adds no alias", NULL == aliases);
    free(name);

    pmix_keep_fqdn_hostnames = saved;
}

/* ------------------------------------------------------------------ */
/* var_dump_color parsing                                              */
/* ------------------------------------------------------------------ */

/* UT_COLORS was placed in the environment before init, so the values
 * below are what pmix_register_params made of it. Every slot must hold a
 * free-able string: the ones named by a good entry hold the assembled
 * escape, and the rest hold "" so pmix_info can emit them unconditionally. */
static void test_var_dump_color(void)
{
    int k;
    bool all_set = true;

    for (k = 0; k < PMIX_VAR_DUMP_COLOR_KEY_COUNT; k++) {
        if (NULL == pmix_var_dump_color[k]) {
            all_set = false;
        }
    }
    report("color:every key has a string", all_set);

    report("color:good entry becomes an escape",
           NULL != pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VAR_NAME] &&
           0 == strcmp(pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VAR_NAME], "\033[34m"));

    /* "value" had no '=' - it is reported and skipped, and the key it
     * names is left uncolored rather than taking a garbage code */
    report("color:malformed entry leaves its key uncolored",
           NULL != pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VAR_VALUE] &&
           0 == strlen(pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VAR_VALUE]));

    /* a good entry after the two bad ones still lands - one bad entry
     * must not abandon the rest of the list */
    report("color:entry after a bad one still applies",
           NULL != pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VALID_VALUES] &&
           0 == strcmp(pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_VALID_VALUES], "\033[36m"));
}

/* ------------------------------------------------------------------ */
/* verbosity output channels                                           */
/* ------------------------------------------------------------------ */

/* pmix_rte_init opens an output channel for every client verbosity var
 * that is set, and pmix_rte_finalize has to give it back. An output
 * descriptor owns a strdup'ed prefix that only pmix_output_close frees,
 * and the id is cached in a file-scope struct that outlives the library
 * - so an unclosed channel both leaks its prefix on every cycle and
 * leaves behind an id naming a slot the next cycle may reissue.
 *
 * pmix_client_get_verbose was set in the environment before init, so
 * this channel is genuinely open. */
static void test_output_channel_open(void)
{
    report("output:a set verbosity opens a channel",
           0 <= pmix_client_globals.get_output);
}

static void test_output_channels_released(const char *when)
{
    char label[128];

    snprintf(label, sizeof(label), "output:channel released (%s)", when);
    report(label, -1 == pmix_client_globals.get_output);

    snprintf(label, sizeof(label), "output:debug channel released (%s)", when);
    report(label, -1 == pmix_globals.debug_output);
}

/* ------------------------------------------------------------------ */
/* IOF flag strings                                                    */
/* ------------------------------------------------------------------ */

/* Anything in the incoming info array that is not one of the handful of
 * bootstrap directives goes to pmix_iof_check_flags, which strdups
 * PMIX_IOF_OUTPUT_TO_FILE / _TO_DIRECTORY into the flag block that
 * pmix_rte_init then copies into pmix_globals. That copy is a bare
 * struct member with no destructor behind it, so pmix_rte_finalize owns
 * the strings - and a host that asks for file output is not exotic. */
static void test_iof_flags_captured(void)
{
    report("iof:file directive captured",
           NULL != pmix_globals.iof_flags.file &&
           0 == strcmp(pmix_globals.iof_flags.file, UT_IOF_FILE));
}

static void test_iof_flags_released(const char *when)
{
    char label[128];

    snprintf(label, sizeof(label), "iof:file string released (%s)", when);
    report(label, NULL == pmix_globals.iof_flags.file);

    snprintf(label, sizeof(label), "iof:directory string released (%s)", when);
    report(label, NULL == pmix_globals.iof_flags.directory);
}

/* ------------------------------------------------------------------ */
/* scalar bootstrap state across the finalize gap                      */
/* ------------------------------------------------------------------ */

/* None of these is re-initialized on the way in - pmix_rte_init only
 * writes them when the matching directive is present - so anything
 * finalize leaves behind silently becomes the next cycle's default.
 * The first cycle here supplies PMIX_NODEID, so a stale value is
 * distinguishable from the "not told" sentinel. */
static void test_scalars_reset(const char *when)
{
    char label[128];

    snprintf(label, sizeof(label), "scalars:nodeid back to sentinel (%s)", when);
    report(label, UINT32_MAX == pmix_globals.nodeid);

    snprintf(label, sizeof(label), "scalars:sessionid back to sentinel (%s)", when);
    report(label, UINT32_MAX == pmix_globals.sessionid);

    /* left set, the next pmix_progress_thread_start declines to spin the
     * engine because it believes the host is driving the loop */
    snprintf(label, sizeof(label), "scalars:external_progress cleared (%s)", when);
    report(label, !pmix_globals.external_progress);

    /* left set, pmix_hwloc_finalize declines to destroy the next
     * cycle's self-discovered topology too */
    snprintf(label, sizeof(label), "scalars:external_topology cleared (%s)", when);
    report(label, !pmix_globals.external_topology);

    snprintf(label, sizeof(label), "scalars:iof formatting cleared (%s)", when);
    report(label, !pmix_globals.iof_flags.set && !pmix_globals.iof_flags.local_output);
}

static void test_nodeid_captured(void)
{
    report("scalars:nodeid directive captured", UT_NODEID == pmix_globals.nodeid);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_info_t info[3];
    pmix_status_t rc;
    uint32_t n;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* all of this has to be in place before the library comes up: the
     * parameters are read by pmix_register_params, and the directives
     * are consumed by pmix_rte_init's directive scan */
    setenv("PMIX_MCA_pmix_var_dump_color", UT_COLORS, 1);
    setenv("PMIX_MCA_pmix_client_get_verbose", "1", 1);
    PMIX_INFO_LOAD(&info[0], PMIX_HOSTNAME, UT_FQDN, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_IOF_OUTPUT_TO_FILE, UT_IOF_FILE, PMIX_STRING);
    n = UT_NODEID;
    PMIX_INFO_LOAD(&info[2], PMIX_NODEID, &n, PMIX_UINT32);

    rc = PMIx_server_init(&mymodule, info, 3);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        PMIX_INFO_DESTRUCT(&info[2]);
        return 1;
    }

    fprintf(stdout, "\n=== runtime bring-up unit tests ===\n\n");

    test_help_topics();
    test_supplied_hostname();
    test_set_aliases();
    test_var_dump_color();
    test_output_channel_open();
    test_iof_flags_captured();
    test_nodeid_captured();

    PMIx_server_finalize();
    test_output_channels_released("first cycle");
    test_iof_flags_released("first cycle");
    test_scalars_reset("first cycle");

    /* and the whole thing again, because everything above is state that
     * has to be rebuilt from scratch. A second cycle that comes up with
     * the same answers is the evidence that finalize left nothing
     * behind - and nothing missing either */
    rc = PMIx_server_init(&mymodule, info, 3);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "second PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n--- second init/finalize cycle ---\n\n");
    test_supplied_hostname();
    test_var_dump_color();
    test_output_channel_open();
    test_iof_flags_captured();
    test_nodeid_captured();

    PMIx_server_finalize();
    test_output_channels_released("second cycle");
    test_iof_flags_released("second cycle");
    test_scalars_reset("second cycle");

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
