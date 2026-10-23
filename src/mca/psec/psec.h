/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for psecurity support. PMIx doesn't need much in
 * this regard, but we do need a mechanism for authenticating connections.
 *
 * Only *one* plugin will be active in a client, but multiple plugins may
 * be active in a server. Thus, this is a multi-select framework.
 *
 * Available plugins may be defined at runtime via the typical MCA parameter
 * syntax.
 */

#ifndef PMIX_PSEC_H
#define PMIX_PSEC_H

#include "src/include/pmix_config.h"

#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/mca/ptl/ptl_types.h"

BEGIN_C_DECLS

/* forward declaration */
struct pmix_peer_t;

/******    MODULE DEFINITION    ******/

/**
 * Initialize the module. Returns an error if the module cannot
 * run, success if it can and wants to be used.
 */
typedef pmix_status_t (*pmix_psec_base_module_init_fn_t)(void);

/**
 * Finalize the module. Tear down any allocated storage, disconnect
 * from any system support (e.g., LDAP server)
 */
typedef void (*pmix_psec_base_module_fini_fn_t)(void);

/****    CLIENT-SIDE FUNCTIONS    ****/
/**
 * Create and return a credential for this client - this
 * could be a string or a byte array, which is why we must
 * also return the length. The directives contain info on
 * desired credential type, or other directives used by
 * the credential agent. The returned info array typically
 * contains the name of the agent issuing the credential,
 * plus any other info the agent chooses to return.
 */
typedef pmix_status_t (*pmix_psec_base_module_create_cred_fn_t)(struct pmix_peer_t *peer,
                                                                const pmix_info_t directives[],
                                                                size_t ndirs, pmix_info_t **info,
                                                                size_t *ninfo,
                                                                pmix_byte_object_t *cred);

/****    SERVER-SIDE FUNCTIONS    ****/
/**
 * Validate a client's credential - required of every module. The
 * credential could be a string
 * or an array of bytes, which is why we include the length. The directives
 * contain information provided by the requestor to aid in the validation
 * process - e.g., the uid/gid of the process seeking validation, or
 * the name of the agent being asked to perform the validation. The
 * returned info array typically contains the name of the agent that
 * actually performed the validation, plus any other info the agent
 * chooses to return (e.g., the uid/gid contained in the credential)
 */
typedef pmix_status_t (*pmix_psec_base_module_validate_cred_fn_t)(struct pmix_peer_t *peer,
                                                                  const pmix_info_t directives[],
                                                                  size_t ndirs, pmix_info_t **info,
                                                                  size_t *ninfo,
                                                                  const pmix_byte_object_t *cred);

/**
 * Whether the module can, as configured, validate a peer's credential -
 * a module may be able to create credentials and still have nothing to
 * check another's against. A server lists only the modules that can
 * when it tells peers which mechanisms it accepts. NULL means always.
 */
typedef bool (*pmix_psec_base_module_validates_fn_t)(void);

/****    BETWEEN HOSTS    ****/
/**
 * A tool on another host and the server it connects to authenticate each
 * other, the server first, and each credential is made for that one
 * connection: it carries `bind`, bytes both ends of the connection know,
 * and is accepted only where those same bytes are expected. `server`
 * says whether the credential is a server's or a tool's, and a credential
 * made as one is never accepted as the other.
 *
 * These are not reached through PMIx_Get_credential or
 * PMIx_Validate_credential: only ptl makes and checks such a credential,
 * and validate_cred refuses one. `to` is the user expected to check it,
 * where the mechanism can say so ((uid_t) -1 for anyone): munge then lets
 * only a process of that user decode it.
 *
 * A module that cannot authenticate a process on another host - one that
 * asks this host's kernel who the peer is - leaves both NULL, and a server
 * refuses a peer on another host that names it.
 */
typedef pmix_status_t (*pmix_psec_base_module_create_bound_fn_t)(const pmix_byte_object_t *bind,
                                                                 bool server, uid_t to,
                                                                 pmix_byte_object_t *cred);

/**
 * Check a credential create_bound made on another host. It must carry
 * exactly `bind` and have been made as a server if `server`, as a tool if
 * not. On success, uid and gid are the identity it establishes.
 */
typedef pmix_status_t (*pmix_psec_base_module_validate_bound_fn_t)(const pmix_byte_object_t *bind,
                                                                   bool server,
                                                                   const pmix_byte_object_t *cred,
                                                                   uid_t *uid, gid_t *gid);

/**
 * Whether the module, as configured, can both make a bound credential and
 * check one - each end of a connection between hosts does both. NULL means
 * always.
 */
typedef bool (*pmix_psec_base_module_mutual_fn_t)(void);

/**
 * Base structure for a PSEC module
 */
typedef struct {
    char *name;
    /* init/finalize */
    pmix_psec_base_module_init_fn_t init;
    pmix_psec_base_module_fini_fn_t finalize;
    /** Client-side */
    pmix_psec_base_module_create_cred_fn_t create_cred;
    /** Server-side */
    pmix_psec_base_module_validate_cred_fn_t validate_cred;
    pmix_psec_base_module_validates_fn_t validates;
    /** Between hosts - both NULL for a module that cannot */
    pmix_psec_base_module_create_bound_fn_t create_bound;
    pmix_psec_base_module_validate_bound_fn_t validate_bound;
    pmix_psec_base_module_mutual_fn_t mutual;
} pmix_psec_module_t;

/* define an API module */

/* get a list of available options - caller must free results
 * when done */
PMIX_EXPORT char *pmix_psec_base_get_available_modules(void);

/* the subset of those that can validate a peer's credential - what a
 * server tells its peers it accepts. Caller must free the result */
PMIX_EXPORT char *pmix_psec_base_get_accepted_modules(void);

/* whether a module can, as configured, authenticate a process on another
 * host and be authenticated by it */
PMIX_EXPORT bool pmix_psec_base_remote_capable(const pmix_psec_module_t *mod);

/* whether one of the active modules can */
PMIX_EXPORT bool pmix_psec_base_remote_available(void);

/* the module to authenticate with a process on another host: `prefer` if
 * it can, else the highest-priority active module that can. NULL if none
 * can */
PMIX_EXPORT pmix_psec_module_t *pmix_psec_base_remote_module(pmix_psec_module_t *prefer);

/* the highest-priority active module that can, of those a comma-delimited
 * list names - a server's, from its rendezvous file. NULL if none */
PMIX_EXPORT pmix_psec_module_t *pmix_psec_base_remote_module_among(const char *list);

/* Select a psec module for a given peer */
PMIX_EXPORT pmix_psec_module_t *pmix_psec_base_assign_module(const char *options);

/* MACROS FOR EXECUTING PSEC FUNCTIONS */

#define PMIX_PSEC_CREATE_CRED(r, p, d, nd, in, nin, c) \
    (r) = (p)->nptr->compat.psec->create_cred((struct pmix_peer_t *) (p), (d), (nd), (in), (nin), c)

#define PMIX_PSEC_VALIDATE_CRED(r, p, d, nd, in, nin, c)                                     \
    (r) = (p)->nptr->compat.psec->validate_cred((struct pmix_peer_t *) (p), (d), (nd), (in), \
                                                (nin), c)

/* Validate a connecting peer with the module "m" it asked for. The
 * connection handler validates before the peer is attached to its
 * namespace, so the module is named here rather than taken from the
 * namespace - which another peer may already have set. The module reads
 * only the peer's info (the identity it was registered or connected as),
 * protocol and socket. */
#define PMIX_PSEC_VALIDATE_CONNECTION_WITH(r, m, p, d, nd, in, nin, c)                             \
    do {                                                                                           \
        pmix_status_t _r;                                                                          \
        /* every module validates a credential; one that cannot is not a                         \
         * security mechanism, and the peer is refused */                                          \
        if (NULL != (m)->validate_cred) {                                                          \
            _r = (m)->validate_cred((struct pmix_peer_t *) (p), (d), (nd), (in), (nin), c);        \
            if (PMIX_SUCCESS != _r) {                                                              \
                pmix_output_verbose(2, pmix_globals.debug_output,                                  \
                                    "validation of credential failed: %s", PMIx_Error_string(_r)); \
            } else {                                                                               \
                pmix_output_verbose(2, pmix_globals.debug_output, "credential validated");         \
            }                                                                                      \
            (r) = _r;                                                                              \
        } else {                                                                                   \
            (r) = PMIX_ERR_NOT_SUPPORTED;                                                          \
        }                                                                                          \
    } while (0)

#define PMIX_PSEC_VALIDATE_CONNECTION(r, p, d, nd, in, nin, c) \
    PMIX_PSEC_VALIDATE_CONNECTION_WITH(r, (p)->nptr->compat.psec, p, d, nd, in, nin, c)

/****    COMPONENT STRUCTURE DEFINITION    ****/

/* define a component-level API for initializing the component */
typedef pmix_status_t (*pmix_psec_base_component_init_fn_t)(void);

/* define a component-level API for finalizing the component */
typedef void (*pmix_psec_base_component_finalize_fn_t)(void);

/* define a component-level API for getting a module */
typedef pmix_psec_module_t *(*pmix_psec_base_component_assign_module_fn_t)(void);

/*
 * the standard component data structure
 */
struct pmix_psec_base_component_t {
    pmix_mca_base_component_t base;
    int priority;
    pmix_psec_base_component_init_fn_t init;
    pmix_psec_base_component_finalize_fn_t finalize;
    pmix_psec_base_component_assign_module_fn_t assign_module;
};
typedef struct pmix_psec_base_component_t pmix_psec_base_component_t;

/* The psec framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PMIX_MCA_BASE_VERSION(psec), and the framework's declaration reaches
 * the same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component built
 * against the previous one would not survive. */
#define PMIX_MCA_psec_MAJOR_VERSION   5
#define PMIX_MCA_psec_MINOR_VERSION   0
#define PMIX_MCA_psec_RELEASE_VERSION 0

END_C_DECLS

#endif
