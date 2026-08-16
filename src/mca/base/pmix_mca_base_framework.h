/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PMIX_MCA_BASE_FRAMEWORK_H)
#    define PMIX_MCA_BASE_FRAMEWORK_H
#    include "src/include/pmix_config.h"

#    include "src/class/pmix_list.h"
#    include "src/mca/mca.h"

/*
 * Register and open flags
 */
enum pmix_mca_base_register_flag_t {
    PMIX_MCA_BASE_REGISTER_DEFAULT = 0,
    /** Register all components (ignore selection MCA variables) */
    PMIX_MCA_BASE_REGISTER_ALL = 1,
    /** Do not register DSO components */
    PMIX_MCA_BASE_REGISTER_STATIC_ONLY = 2
};

typedef enum pmix_mca_base_register_flag_t pmix_mca_base_register_flag_t;

enum pmix_mca_base_open_flag_t {
    PMIX_MCA_BASE_OPEN_DEFAULT = 0,
    /** Find components in pmix_mca_base_components_find. Used by
     pmix_mca_base_framework_open() when NOREGISTER is specified
     by the framework */
    PMIX_MCA_BASE_OPEN_FIND_COMPONENTS = 1,
    /** Do not open DSO components */
    PMIX_MCA_BASE_OPEN_STATIC_ONLY = 2,
};

typedef enum pmix_mca_base_open_flag_t pmix_mca_base_open_flag_t;

/**
 * Register the MCA framework parameters
 *
 * @param[in] flags Registration flags (see mca/base/pmix_base.h)
 *
 * @retval PMIX_SUCCESS on success
 * @retval pmix error code on failure
 *
 * This function registers all framework MCA parameters. This
 * function should not call pmix_mca_base_framework_components_register().
 *
 * Frameworks are NOT required to provide this function. It
 * may be NULL.
 */
typedef int (*pmix_mca_base_framework_register_params_fn_t)(pmix_mca_base_register_flag_t flags);

/**
 * Initialize the MCA framework
 *
 * @retval PMIX_SUCCESS Upon success
 * @retval PMIX_ERROR Upon failure
 *
 * This must be the first function invoked in the MCA framework.
 * It initializes the MCA framework, finds and opens components,
 * populates the components list, etc.
 *
 * This function is invoked during pmix_init() and during the
 * initialization of the special case of the ompi_info command.
 *
 * This function fills in the components framework value, which
 * is a list of all components that were successfully opened.
 * This variable should \em only be used by other framework base
 * functions or by ompi_info -- it is not considered a public
 * interface member -- and is only mentioned here for completeness.
 *
 * Any resources allocated by this function must be freed
 * in the framework close function.
 *
 * Frameworks are NOT required to provide this function. It may
 * be NULL. If a framework does not provide an open function the
 * default behavior of pmix_mca_base_framework_open() is to call
 * pmix_mca_base_framework_components_open(). If a framework provides
 * an open function it will need to call pmix_mca_base_framework_components_open()
 * if it needs to open any components.
 */
typedef int (*pmix_mca_base_framework_open_fn_t)(pmix_mca_base_open_flag_t flags);

/**
 * Shut down the MCA framework.
 *
 * @retval PMIX_SUCCESS Always
 *
 * This function should shut downs everything in the MCA
 * framework, and is called during pmix_finalize() and the
 * special case of the ompi_info command.
 *
 * It must be the last function invoked on the MCA framework.
 *
 * Frameworks are NOT required to provide this function. It may
 * be NULL. If a framework does not provide a close function the
 * default behavior of pmix_mca_base_framework_close() is to call
 * pmix_mca_base_framework_components_close(). If a framework provide
 * a close function it will need to call pmix_mca_base_framework_components_close()
 * if any components were opened.
 */
typedef int (*pmix_mca_base_framework_close_fn_t)(void);

typedef enum {
    PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT = 0,
    /** Don't register any variables for this framework */
    PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER = 1,
    /** Internal. Don't set outside pmix_mca_base_framework.h */
    PMIX_MCA_BASE_FRAMEWORK_FLAG_REGISTERED = 2,
    /** Framework does not have any DSO components */
    PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO = 4,
    /** Internal. Don't set outside pmix_mca_base_framework.h */
    PMIX_MCA_BASE_FRAMEWORK_FLAG_OPEN = 8,
    /**
     * The upper 16 bits are reserved for project specific flags.
     */
} pmix_mca_base_framework_flags_t;

typedef struct pmix_mca_base_framework_t {
    /** Project name for this component (ex "pmix") */
    char *framework_project;
    /** Framework name */
    char *framework_name;
    /** The framework interface version this build of the framework
        speaks. A component records the same pair in its own struct, and
        open_components() refuses one that does not match: a component is
        a run-time-loadable plugin, so an installed one can be older than
        the library loading it, and nothing else in the MCA compares
        these.

        Neither the framework nor its components state these numbers
        directly. Both read them from the PMIX_MCA_<name>_*_VERSION
        macros in the framework's own header, so there is exactly one
        place to edit when the module interface changes - see
        PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE below.

        Zero means "this framework declares no interface version", which
        is what the unversioned PMIX_MCA_BASE_FRAMEWORK_DECLARE produces
        and what every framework outside this project therefore gets. The
        check is skipped for such a framework rather than refusing
        everything it owns. */
    int framework_type_major_version;
    int framework_type_minor_version;
    /** Description of this framework or NULL */
    const char *framework_description;
    /** Framework register function or NULL if the framework
        and all its components have nothing to register */
    pmix_mca_base_framework_register_params_fn_t framework_register;
    /** Framework open function or NULL */
    pmix_mca_base_framework_open_fn_t framework_open;
    /** Framework close function or NULL */
    pmix_mca_base_framework_close_fn_t framework_close;
    /** Framework flags (future use) set to 0 */
    pmix_mca_base_framework_flags_t framework_flags;
    /** Framework open count */
    int framework_refcnt;
    /** List of static components */
    const pmix_mca_base_component_t ***framework_static_components;
    /** Component selection. This will be registered with the MCA
        variable system and should be either NULL (all components) or
        a heap allocated, comma-delimited list of components. */
    char *framework_selection;
    /** Verbosity level (0-100) */
    int framework_verbose;
    /** Pmix output for this framework (or -1) */
    int framework_output;
    /** List of selected components (filled in by pmix_mca_base_framework_register()
        or pmix_mca_base_framework_open() */
    pmix_list_t framework_components;
    /** List of components that failed to load */
    pmix_list_t framework_failed_components;
} pmix_mca_base_framework_t;

/**
 * Register a framework with MCA.
 *
 * @param[in] framework framework to register
 *
 * @retval PMIX_SUCCESS Upon success
 * @retval PMIX_ERROR Upon failure
 *
 * Call a framework's register function.
 */
PMIX_EXPORT int pmix_mca_base_framework_register(pmix_mca_base_framework_t *framework,
                                                 pmix_mca_base_register_flag_t flags);

/**
 * Open a framework
 *
 * @param[in] framework framework to open
 *
 * @retval PMIX_SUCCESS Upon success
 * @retval PMIX_ERROR Upon failure
 *
 * Call a framework's open function.
 */
PMIX_EXPORT int pmix_mca_base_framework_open(pmix_mca_base_framework_t *framework,
                                             pmix_mca_base_open_flag_t flags);

/**
 * Close a framework
 *
 * @param[in] framework framework to close
 *
 * @retval PMIX_SUCCESS Upon success
 * @retval PMIX_ERROR Upon failure
 *
 * Call a framework's close function.
 */
PMIX_EXPORT int pmix_mca_base_framework_close(pmix_mca_base_framework_t *framework);

/**
 * Check if a framework is already registered
 *
 * @param[in] framework framework to query
 *
 * @retval true if the framework's mca variables are registered
 * @retval false if not
 */
PMIX_EXPORT bool pmix_mca_base_framework_is_registered(struct pmix_mca_base_framework_t *framework);

/**
 * Check if a framework is already open
 *
 * @param[in] framework framework to query
 *
 * @retval true if the framework is open
 * @retval false if not
 */
PMIX_EXPORT bool pmix_mca_base_framework_is_open(struct pmix_mca_base_framework_t *framework);

/**
 * Macro to declare an MCA framework, stating an interface version
 *
 * Identical to PMIX_MCA_BASE_FRAMEWORK_DECLARE, except that the
 * framework's interface version is picked up from the
 * PMIX_MCA_<name>_MAJOR_VERSION / _MINOR_VERSION macros its own header
 * defines, reached by pasting the framework's name through
 * PMIX_MCA_FW_VER() in mca.h - the same three integers PMIX_MCA_BASE_VERSION()
 * stamps into every one of its components. It takes no version arguments
 * precisely so that a version bump is one edit, in one header, and never
 * reaches this declaration.
 *
 * A framework that uses this macro without defining those two macros
 * does not compile, which is the point: within this project a framework
 * states its version or says so loudly.
 *
 * Example:
 *  PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, foo, NULL, pmix_foo_open,
 *                                            pmix_foo_close, NULL,
 *                                            MCA_BASE_FRAMEWORK_FLAG_LAZY)
 */
#    define PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(project, name, description, registerfn,   \
                                                     openfn, closefn, static_components, flags) \
        PMIX_MCA_BASE_FRAMEWORK_DECLARE_FULL(project, name, PMIX_MCA_FW_VER(name, MAJOR),       \
                                             PMIX_MCA_FW_VER(name, MINOR), description,         \
                                             registerfn, openfn, closefn, static_components,    \
                                             flags)

/**
 * Macro to declare an MCA framework
 *
 * This is the original, unversioned form, and its signature is frozen:
 * it is reached through an installed header and companion projects -
 * PRRTE declares every one of its frameworks with it - so it must keep
 * compiling for a caller that knows nothing about interface versions.
 * Such a framework reports version 0.0, which open_components() reads as
 * "no version stated" and skips the component check for.
 *
 * Frameworks inside this project should use
 * PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE instead.
 *
 * Example:
 *  PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, foo, NULL, pmix_foo_open, pmix_foo_close,
 * MCA_BASE_FRAMEWORK_FLAG_LAZY)
 */
#    define PMIX_MCA_BASE_FRAMEWORK_DECLARE(project, name, description, registerfn, openfn,  \
                                            closefn, static_components, flags)               \
        PMIX_MCA_BASE_FRAMEWORK_DECLARE_FULL(project, name, 0, 0, description, registerfn,    \
                                             openfn, closefn, static_components, flags)

/**
 * The declaration both of the above expand to.
 *
 * Frameworks in this project use one of the two macros above rather than
 * this one. A companion project is the exception, and a sanctioned one:
 * the versioned form reaches its numbers by pasting a PMIX_ prefix, which
 * is this project's namespace, so a project whose frameworks state their
 * own versions builds its equivalent on top of this - PRRTE's
 * PRTE_MCA_BASE_FRAMEWORK_DECLARE pastes PRTE_MCA_<name>_*_VERSION and
 * hands the result here. Treat the argument list as part of the installed
 * interface for that reason.
 *
 * Such a project has to be able to ask whether the PMIx it is being built
 * against offers this, because the failure otherwise lands as a syntax
 * error inside its own headers - the version numbers arrive where an
 * older macro expected something else. PMIX_CAP_MCA_FW_VERSION is that
 * question: it is defined from the release that carries both this macro
 * and the PMIX_MCA_BASE_VERSION_2_1_0 stamp its components need.
 */
#    define PMIX_MCA_BASE_FRAMEWORK_DECLARE_FULL(project, name, type_major, type_minor,     \
                                                 description, registerfn, openfn,           \
                                                 closefn, static_components, flags)         \
        pmix_mca_base_framework_t project##_##name##_base_framework                         \
            = {.framework_project = #project,                                               \
               .framework_name = #name,                                                     \
               .framework_type_major_version = type_major,                                  \
               .framework_type_minor_version = type_minor,                                  \
               .framework_description = description,                                        \
               .framework_register = registerfn,                                            \
               .framework_open = openfn,                                                    \
               .framework_close = closefn,                                                  \
               .framework_flags = flags,                                                    \
               .framework_refcnt = 0,                                                       \
               .framework_static_components = static_components,                            \
               .framework_selection = NULL,                                                 \
               .framework_verbose = 0,                                                      \
               .framework_output = -1}

#endif /* PMIX_MCA_BASE_FRAMEWORK_H */
