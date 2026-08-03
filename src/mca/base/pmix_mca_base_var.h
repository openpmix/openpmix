/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
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
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 * This file presents the MCA variable interface.
 *
 * Note that there are two scopes for MCA variables: "normal" and
 * attributes.  Specifically, all MCA variables are "normal" -- some
 * are special and may also be found on attributes on communicators,
 * datatypes, or windows.
 *
 * In general, these functions are intended to be used as follows:
 *
 * - Creating MCA variables
 * -# Register a variable, get an index back
 * - Using MCA variables
 * -# Lookup a "normal" variable value on a specific index, or
 * -# Lookup an attribute variable on a specific index and
 *    communicator / datatype / window.
 *
 * MCA variables can be defined in multiple different places.  As
 * such, variables are \em resolved to find their value.  The order
 * of resolution is as follows:
 *
 * - An "override" location that is only available to be set via the
 *   pmix_mca_base_param API.
 * - Look for an environment variable corresponding to the MCA
 *   variable.
 * - See if a file contains the MCA variable (MCA variable files are
 *   read only once -- when the first time any mca_param_t function is
 *   invoked).
 * - If nothing else was found, use the variable's default value.
 *
 * Note that there is a second header file (pmix_mca_base_vari.h)
 * that contains several internal type declarations for the variable
 * system.  The internal file is only used within the variable system
 * itself; it should not be required by any other PMIX entities.
 */

#ifndef PMIX_MCA_BASE_VAR_H
#define PMIX_MCA_BASE_VAR_H

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_value_array.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var_enum.h"
#include "src/mca/base/pmix_mca_base_var_group.h"
#include "src/mca/mca.h"

/**
 * The types of MCA variables.
 */
typedef enum {
    /** The variable is of type int. */
    PMIX_MCA_BASE_VAR_TYPE_INT,
    /** The variable is of type unsigned int */
    PMIX_MCA_BASE_VAR_TYPE_UNSIGNED_INT,
    /** The variable is of type unsigned long */
    PMIX_MCA_BASE_VAR_TYPE_UNSIGNED_LONG,
    /** The variable is of type unsigned long long */
    PMIX_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG,
    /** The variable is of type size_t */
    PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
    /** The variable is of type string. */
    PMIX_MCA_BASE_VAR_TYPE_STRING,
    /** The variable is of type string and contains version. */
    PMIX_MCA_BASE_VAR_TYPE_VERSION_STRING,
    /** The variable is of type bool */
    PMIX_MCA_BASE_VAR_TYPE_BOOL,
    /** The variable is of type double */
    PMIX_MCA_BASE_VAR_TYPE_DOUBLE,
    /** Maximum variable type. */
    PMIX_MCA_BASE_VAR_TYPE_MAX
} pmix_mca_base_var_type_t;

PMIX_EXPORT extern const char *pmix_var_type_names[];

/**
 * Source of an MCA variable's value
 */
typedef enum {
    /** The default value */
    PMIX_MCA_BASE_VAR_SOURCE_DEFAULT,
    /** The value came from the command line */
    PMIX_MCA_BASE_VAR_SOURCE_COMMAND_LINE,
    /** The value came from the environment */
    PMIX_MCA_BASE_VAR_SOURCE_ENV,
    /** The value came from a file */
    PMIX_MCA_BASE_VAR_SOURCE_FILE,
    /** The value came a "set" API call */
    PMIX_MCA_BASE_VAR_SOURCE_SET,
    /** The value came from the override file */
    PMIX_MCA_BASE_VAR_SOURCE_OVERRIDE,

    /** Maximum source type */
    PMIX_MCA_BASE_VAR_SOURCE_MAX
} pmix_mca_base_var_source_t;

typedef enum {
    PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED = 0x0001
} pmix_mca_base_var_syn_flag_t;

typedef enum {
    /** Variable is internal (hidden from *_info) */
    PMIX_MCA_BASE_VAR_FLAG_INTERNAL = 0x0001,
    /** Variable will always be the default value. Implies
        !PMIX_MCA_BASE_VAR_FLAG_SETTABLE */
    PMIX_MCA_BASE_VAR_FLAG_DEFAULT_ONLY = 0x0002,
    /** Variable can be set with mca_base_var_set() */
    PMIX_MCA_BASE_VAR_FLAG_SETTABLE = 0x0004,
    /** Variable is deprecated */
    PMIX_MCA_BASE_VAR_FLAG_DEPRECATED = 0x0008,
    /** Variable has been overridden */
    PMIX_MCA_BASE_VAR_FLAG_OVERRIDE = 0x0010,
    /** Variable may not be set from a file */
    PMIX_MCA_BASE_VAR_FLAG_ENVIRONMENT_ONLY = 0x0020,
    /** Variable should be deregistered when the group is deregistered
        (DWG = "deregister with group").  This flag is set
        automatically when you register a variable with
        mca_base_component_var_register(), but can also be set
        manually when you register a variable with
        mca_base_var_register().  Analogous to the
        MCA_BASE_PVAR_FLAG_IWG. */
    PMIX_MCA_BASE_VAR_FLAG_DWG = 0x0040,
    /** Variable has a default value of "unset". Meaning to only
     * be set when the user explicitly asks for it */
    PMIX_MCA_BASE_VAR_FLAG_DEF_UNSET = 0x0080,
} pmix_mca_base_var_flag_t;

typedef enum {
    PMIX_MCA_BASE_VAR_FLAG_NONE = 0x0000,
    /** Variable is valid */
    PMIX_MCA_BASE_VAR_FLAG_VALID = 0x00010000,
    /** Variable is a synonym */
    PMIX_MCA_BASE_VAR_FLAG_SYNONYM = 0x00020000,
    /** mbv_source_file needs to be freed */
    PMIX_MCA_BASE_VAR_FLAG_SOURCE_FILE_NEEDS_FREE = 0x00040000
} pmix_mca_base_var_flag_internal_t;

/**
 * Types for MCA parameters.
 */
typedef union {
    /** integer value */
    int intval;
    /** unsigned int value */
    unsigned int uintval;
    /** string value */
    char *stringval;
    /** boolean value */
    bool boolval;
    /** unsigned long value */
    unsigned long ulval;
    /** unsigned long long value */
    unsigned long long ullval;
    /** size_t value */
    size_t sizetval;
    /** double value */
    double lfval;
} pmix_mca_base_var_storage_t;

/**
 * Entry for holding information about an MCA variable.
 */
struct pmix_mca_base_var_t {
    /** Allow this to be an PMIX OBJ */
    pmix_object_t super;

    /** Variable index. This will remain constant until pmix_mca_base_var_finalize()
        is called. */
    int mbv_index;
    /** Group index. This will remain constant until pmix_mca_base_var_finalize()
        is called. This variable will be deregistered if the associated group
        is deregistered with pmix_mca_base_var_group_deregister() */
    int mbv_group_index;

    /** Enum indicating the type of the variable (integer, string, boolean) */
    pmix_mca_base_var_type_t mbv_type;

    /** String of the variable name */
    char *mbv_variable_name;
    /** Full variable name, in case it is not <framework>_<component>_<param> */
    char *mbv_full_name;
    /** Long variable name <project>_<framework>_<component>_<name> */
    char *mbv_long_name;
    char *mbv_prefix;

    /** List of synonym names for this variable.  This *must* be a
        pointer (vs. a plain pmix_list_t) because we copy this whole
        struct into a new var for permanent storage
        (pmix_vale_array_append_item()), and the internal pointers in
        the pmix_list_t will be invalid when that happens.  Hence, we
        simply keep a pointer to an external pmix_list_t.  Synonyms
        are uncommon enough that this is not a big performance hit. */
    pmix_value_array_t mbv_synonyms;

    /** Variable flags */
    pmix_mca_base_var_flag_internal_t mbv_flags;

    /** Source of the current value */
    pmix_mca_base_var_source_t mbv_source;

    /** Synonym for */
    int mbv_synonym_for;

    /** Variable description */
    char *mbv_description;

    /** File the value came from */
    char *mbv_source_file;

    /** Value enumerator (only valid for integer variables) */
    pmix_mca_base_var_enum_t *mbv_enumerator;

    /** Bind value for this variable (0 - none) */
    int mbv_bind;

    /** Storage for this variable */
    pmix_mca_base_var_storage_t *mbv_storage;

    /** File value structure */
    void *mbv_file_value;
};
/**
 * Convenience typedef.
 */
typedef struct pmix_mca_base_var_t pmix_mca_base_var_t;

/*
 * Global functions for MCA
 */

BEGIN_C_DECLS

/**
 * Object declarayion for pmix_mca_base_var_t
 */
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_mca_base_var_t);

/**
 * Initialize the MCA variable system.
 *
 * @retval PMIX_SUCCESS
 *
 * This function initializes the MCA variable system.  It is
 * invoked internally (by pmix_mca_base_open()) and is only documented
 * here for completeness.
 */
PMIX_EXPORT int pmix_mca_base_var_init(void);

/**
 * Register an MCA variable
 *
 * @param[in] project_name The name of the project associated with
 * this variable
 * @param[in] framework_name The name of the framework associated with
 * this variable
 * @param[in] component_name The name of the component associated with
 * this variable
 * @param[in] variable_name The name of this variable
 * @param[in] description A string describing the use and valid
 * values of the variable (string).
 * @param[in] type The type of this variable (string, int, bool).
 * @param[in,out] storage Pointer to the value's location.
 *
 * @retval index Index value representing this variable. NOTE: this is
 * an index, not a status -- a successful registration returns a value
 * >= 0, and index 0 is a perfectly ordinary result that happens to
 * equal PMIX_SUCCESS. Test the sign (0 > rc), never (PMIX_SUCCESS != rc).
 * @retval PMIX_ERR_OUT_OF_RESOURCE Upon failure to allocate memory.
 * @retval PMIX_ERROR Upon failure to register the variable.
 *
 * This function registers an MCA variable and associates it
 * with a specific group.
 *
 * {description} is a string of arbitrary length (verbose is good!)
 * for explaining what the variable is for and what its valid values
 * are.  This message is used in help messages, such as the output
 * from the pmix_info executable.  The {description} string is copied
 * internally; the caller can free {description} upon successful
 * return.
 *
 * {storage} points to a (char *), (int), or (bool) where the value of
 * this variable is stored ({type} indicates the type of this
 * pointer).  The location pointed to by {storage} must exist until
 * the variable is deregistered.  The registration itself resolves the
 * value and writes it through {storage}, so the caller's variable is
 * correct as soon as this function returns -- there is no separate
 * "look it up" step.  Note that the initial value in {storage} is
 * therefore overwritten whenever the user set this variable through
 * the environment or a parameter file.  If the input value of
 * {storage} points to a (char *), the pointed-to string will be
 * duplicated and maintained internally by the MCA variable system;
 * the caller may free the original string after this function returns
 * successfully.
 *
 * NOTE: this interface takes no flags, scope, info level, binding hint
 * or enumerator. Earlier revisions of this comment described all five,
 * and none of them has ever been a parameter of this function. The
 * corresponding pmix_mca_base_var_t fields (mbv_flags beyond the
 * internal bits, mbv_bind, mbv_enumerator) are consequently never set
 * by any caller in this tree -- the enumerator machinery in
 * pmix_mca_base_var_enum.h is reachable only by calling it directly.
 * Do not write a call against the removed parameters; it will not
 * compile.
 */
PMIX_EXPORT int pmix_mca_base_var_register(const char *project_name, const char *framework_name,
                                           const char *component_name, const char *variable_name,
                                           const char *description, pmix_mca_base_var_type_t type,
                                           void *storage);

/**
 * Convenience function for registering a variable associated with a
 * component.
 *
 * While quite similar to pmix_mca_base_var_register(), there is one key
 * difference: vars registered this this function will automatically
 * be unregistered / made unavailable when that component is closed by
 * its framework.
 */
PMIX_EXPORT int pmix_mca_base_component_var_register(
    const pmix_mca_base_component_t *component, const char *variable_name, const char *description,
    pmix_mca_base_var_type_t type, void *storage);

/**
 * Convenience function for registering a variable associated with a framework. This
 * function is equivalent to pmix_mca_base_var_register with component_name = "base" and
 * with the MCA_BASE_VAR_FLAG_DWG set. See pmix_mca_base_var_register().
 */
PMIX_EXPORT int pmix_mca_base_framework_var_register(
    const pmix_mca_base_framework_t *framework, const char *variable_name, const char *help_msg,
    pmix_mca_base_var_type_t type, void *storage);

/**
 * Register a synonym name for an MCA variable.
 *
 * @param[in] synonym_for The index of the original variable. This index
 * must not refer to a synonym.
 * @param[in] project_name The project this synonym belongs to. Should
 * not be NULL (except for legacy reasons).
 * @param[in] framework_name The framework this synonym belongs to.
 * @param[in] component_name The component this synonym belongs to.
 * @param[in] synonym_name The synonym name.
 * @param[in] flags Flags for this synonym.
 *
 * @returns index Variable index for new synonym on success.
 * @returns PMIX_ERR_BAD_VAR If synonym_for does not reference a valid
 * variable.
 * @returns PMIX_ERR_OUT_OF_RESOURCE If memory could not be allocated.
 * @returns PMIX_ERROR For all other errors.
 *
 * Upon success, this function creates a synonym MCA variable
 * that will be treated almost exactly like the original.  The
 * type (int or string) is irrelevant; this function simply
 * creates a new name that by which the same variable value is
 * accessible.
 *
 * Note that the original variable name has precedence over all
 * synonyms.  For example, consider the case if variable is
 * originally registered under the name "A" and is later
 * registered with synonyms "B" and "C".  If the user sets values
 * for both MCA variable names "A" and "B", the value associated
 * with the "A" name will be used and the value associated with
 * the "B" will be ignored (and will not even be visible by the
 * pmix_mca_base_var_*() API).  If the user sets values for both MCA
 * variable names "B" and "C" (and does *not* set a value for
 * "A"), it is undefined as to which value will be used.
 */
PMIX_EXPORT int pmix_mca_base_var_register_synonym(int synonym_for, const char *project_name,
                                                   const char *framework_name,
                                                   const char *component_name,
                                                   const char *synonym_name,
                                                   pmix_mca_base_var_syn_flag_t flags);

/**
 * Deregister a MCA variable or synonym
 *
 * @param vari Index returned from pmix_mca_base_var_register() or
 * pmix_mca_base_var_register_synonym().
 *
 * Deregistering a variable does not free the variable or any memory associated
 * with it. All memory will be freed and the variable index released when
 * pmix_mca_base_var_finalize() is called.
 *
 * If an enumerator is associated with this variable it will be dereferenced.
 */
PMIX_EXPORT int pmix_mca_base_var_deregister(int vari);

/**
 * Get the current value of an MCA variable.
 *
 * @param[in] vari Index of variable
 * @param[out] value Address of a pointer that will be made to point at
 * the variable's backing store. Can be NULL.
 * @param[out] source Source of current value. Can be NULL.
 * @param[out] source_file Source file for the current value if
 * it was set from a file.
 *
 * @return PMIX_ERROR Upon failure.  The contents of value are
 * undefined.
 * @return PMIX_SUCCESS Upon success.
 *
 * IMPORTANT: {value} does NOT receive a copy of the value. It receives
 * a *pointer to the storage the caller supplied at registration* --
 * that is, the argument must be the address of a pointer of the
 * variable's type, and the value is read by dereferencing what comes
 * back:
 *
 *   int *val;
 *   pmix_mca_base_var_get_value(idx, &val, NULL, NULL);
 *   printf("%d\n", *val);
 *
 * Passing the address of an int (rather than of an int *) writes a
 * pointer through it. An earlier revision of this comment described a
 * copy-out interface with a {value_size} parameter; no such parameter
 * has ever existed, and no copy is made.
 *
 * Because what comes back aliases the registering code's own storage,
 * it also tracks any change that code makes directly -- so {source}
 * may not reflect where the current contents actually came from.
 */
PMIX_EXPORT int pmix_mca_base_var_get_value(int vari, void *value,
                                            pmix_mca_base_var_source_t *source,
                                            const char **source_file);

/**
 * Find the index for an MCA variable based on its names.
 *
 * @param project_name   Name of the project
 * @param type_name      Name of the type containing the variable.
 * @param component_name Name of the component containing the variable.
 * @param param_name     Name of the variable.
 *
 * @retval PMIX_ERROR If the variable was not found.
 * @retval vari If the variable was found.
 *
 * It is not always convenient to widely propagate a variable's index
 * value, or it may be necessary to look up the variable from a
 * different component. This function can be used to look up the index
 * of any registered variable.  The returned index can be used with
 * pmix_mca_base_var_get() and pmix_mca_base_var_get_value().
 */
PMIX_EXPORT int pmix_mca_base_var_find(const char *project_name, const char *type_name,
                                       const char *component_name, const char *param_name);

/**
 * Find the index for a variable based on its full name
 *
 * @param full_name [in] Full name of the variable
 * @param vari [out]    Index of the variable
 *
 * See pmix_mca_base_var_find().
 */
PMIX_EXPORT int pmix_mca_base_var_find_by_name(const char *full_name, int *vari);

/**
 * Check that two MCA variables were not both set to non-default
 * values.
 *
 * @param type_a [in] Framework name of variable A (string).
 * @param component_a [in] Component name of variable A (string).
 * @param param_a [in] Variable name of variable A (string.
 * @param type_b [in] Framework name of variable A (string).
 * @param component_b [in] Component name of variable A (string).
 * @param param_b [in] Variable name of variable A (string.
 *
 * This function is useful for checking that the user did not set both
 * of 2 mutually-exclusive MCA variables.
 *
 * This function will print an pmix_show_help() message and return
 * PMIX_ERR_BAD_VAR if it finds that the two variables both have
 * value sources that are not MCA_BASE_VAR_SOURCE_DEFAULT.  This
 * means that both variables have been set by the user (i.e., they're
 * not default values).
 *
 * Note that pmix_show_help() allows itself to be hooked, so if this
 * happens after the aggregated pmix_show_help() system is
 * initialized, the messages will be aggregated (w00t).
 *
 * @returns PMIX_ERR_BAD_VAR if the two variables have sources that
 * are not MCA_BASE_VAR_SOURCE_DEFAULT.
 * @returns PMIX_SUCCESS otherwise.
 */
PMIX_EXPORT int pmix_mca_base_var_check_exclusive(const char *project, const char *type_a,
                                                  const char *component_a, const char *param_a,
                                                  const char *type_b, const char *component_b,
                                                  const char *param_b);

/**
 * Obtain basic info on a single variable (name, help message, etc)
 *
 * @param[in] vari Valid variable index.
 * @param[out] var Storage for the variable pointer.
 *
 * @retval PMIX_SUCCESS Upon success.
 * @retval pmix error code Upon failure.
 *
 * The returned pointer belongs to the MCA variable system. Do not
 * modify/free/retain the pointer.
 */
PMIX_EXPORT int pmix_mca_base_var_get(int vari, const pmix_mca_base_var_t **var);

/**
 * Obtain the number of variables that have been registered.
 *
 * @retval count on success
 * @return pmix error code on error
 *
 * Note: This function does not return the number of valid MCA variables as
 * pmix_mca_base_var_deregister() has no impact on the variable count. The count
 * returned is equal to the number of calls to pmix_mca_base_var_register with
 * unique names. ie. two calls with the same name will not affect the count.
 */
PMIX_EXPORT int pmix_mca_base_var_get_count(void);

/**
 * Obtain a list of environment variables describing the all
 * valid (non-default) MCA variables and their sources.
 *
 * @param[out] env A pointer to an argv-style array of key=value
 * strings, suitable for use in an environment
 * @param[out] num_env A pointer to an int, containing the length
 * of the env array (not including the final NULL entry).
 *
 * @retval PMIX_SUCCESS Upon success.
 * @retval PMIX_ERROR Upon failure.
 *
 * This function is similar to pmix_mca_base_var_dump() except that
 * its output is in terms of an argv-style array of key=value
 * strings, suitable for using in an environment.
 */
PMIX_EXPORT int pmix_mca_base_var_build_env(char ***env, int *num_env);

/**
 * Shut down the MCA variable system (normally only invoked by the
 * MCA framework itself).
 *
 * @returns PMIX_SUCCESS This function never fails.
 *
 * This function shuts down the MCA variable repository and frees all
 * associated memory.  No other pmix_mca_base_var*() functions can be
 * invoked after this function.
 *
 * This function is normally only invoked by the MCA framework itself
 * when the process is shutting down (e.g., during MPI_FINALIZE).  It
 * is only documented here for completeness.
 */
PMIX_EXPORT int pmix_mca_base_var_finalize(void);

typedef enum {
    /* Dump human-readable strings */
    PMIX_MCA_BASE_VAR_DUMP_READABLE = 0,
    /* Dump easily parsable strings */
    PMIX_MCA_BASE_VAR_DUMP_PARSABLE = 1,
    /* Dump simple name=value string */
    PMIX_MCA_BASE_VAR_DUMP_SIMPLE = 2,
    /* Dump in color */
    PMIX_MCA_BASE_VAR_DUMP_READABLE_COLOR = 3
} pmix_mca_base_var_dump_type_t;

/* Supported color configuration keys
 * for dumping MCA variables with colors */
typedef enum {
    PMIX_VAR_DUMP_COLOR_VAR_NAME = 0,
    PMIX_VAR_DUMP_COLOR_VAR_VALUE = 1,
    PMIX_VAR_DUMP_COLOR_VALID_VALUES = 2,
    PMIX_VAR_DUMP_COLOR_KEY_COUNT
} pmix_var_dump_color_key_t;

PMIX_EXPORT extern char *pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_KEY_COUNT];


/**
 * Dump strings describing the MCA variable at an index.
 *
 * @param[in]  vari        Variable index
 * @param[out] out         Array of strings describing this variable
 * @param[in]  output_type Type of output desired
 *
 * This function returns an array of strings describing the variable. All strings
 * and the array must be freed by the caller.
 */
PMIX_EXPORT int pmix_mca_base_var_dump(int vari, char ***out,
                                       pmix_mca_base_var_dump_type_t output_type);

#define MCA_COMPILETIME_VER "print_compiletime_version"
#define MCA_RUNTIME_VER     "print_runtime_version"

PMIX_EXPORT int pmix_mca_base_var_cache_files(bool rel_path_search);

END_C_DECLS

#endif /* PMIX_MCA_BASE_VAR_H */
