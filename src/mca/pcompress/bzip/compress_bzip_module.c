/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 *
 * Copyright (c) 2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */

#include "src/util/pmix_environ.h"
#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/printf.h"

#include "pmix_common.h"
#include "src/util/basename.h"

#include "src/mca/compress/compress.h"
#include "src/mca/compress/base/base.h"

#include "compress_bzip.h"

static bool is_directory(char *fname );

int pmix_compress_bzip_module_init(void)
{
    return PMIX_SUCCESS;
}

int pmix_compress_bzip_module_finalize(void)
{
    return PMIX_SUCCESS;
}

int pmix_compress_bzip_compress(char * fname, char **cname, char **postfix)
{
    pid_t child_pid = 0;
    int status = 0;

    pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                        "compress:bzip: compress(%s)",
                        fname);

    pmix_compress_bzip_compress_nb(fname, cname, postfix, &child_pid);
    waitpid(child_pid, &status, 0);

    if( WIFEXITED(status) ) {
        return PMIX_SUCCESS;
    } else {
        return PMIX_ERROR;
    }
}

int pmix_compress_bzip_compress_nb(char * fname, char **cname, char **postfix, pid_t *child_pid)
{
    char **argv = NULL;
    char * base_fname = NULL;
    char * dir_fname = NULL;
    int status;
    bool is_dir;

    is_dir = is_directory(fname);

    *child_pid = fork();
    if( *child_pid == 0 ) { /* Child */
        char * cmd;

        dir_fname  = pmix_dirname(fname);
        base_fname = pmix_basename(fname);

        chdir(dir_fname);

        if( is_dir ) {
            pmix_asprintf(cname, "%s.tar.bz2", base_fname);
            pmix_asprintf(&cmd, "tar -jcf %s %s", *cname, base_fname);
        } else {
            pmix_asprintf(cname, "%s.bz2", base_fname);
            pmix_asprintf(&cmd, "bzip2 %s", base_fname);
        }

        pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                            "compress:bzip: compress_nb(%s -> [%s])",
                            fname, *cname);
        pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                            "compress:bzip: compress_nb() command [%s]",
                            cmd);

        argv = pmix_argv_split(cmd, ' ');
        status = execvp(argv[0], argv);

        pmix_output(0, "compress:bzip: compress_nb: Failed to exec child [%s] status = %d\n", cmd, status);
        exit(PMIX_ERROR);
    }
    else if( *child_pid > 0 ) {
        if( is_dir ) {
            *postfix = strdup(".tar.bz2");
        } else {
            *postfix = strdup(".bz2");
        }
        pmix_asprintf(cname, "%s%s", fname, *postfix);
    }
    else {
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

int pmix_compress_bzip_decompress(char * cname, char **fname)
{
    pid_t child_pid = 0;
    int status = 0;

    pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                        "compress:bzip: decompress(%s)",
                        cname);

    pmix_compress_bzip_decompress_nb(cname, fname, &child_pid);
    waitpid(child_pid, &status, 0);

    if( WIFEXITED(status) ) {
        return PMIX_SUCCESS;
    } else {
        return PMIX_ERROR;
    }
}

int pmix_compress_bzip_decompress_nb(char * cname, char **fname, pid_t *child_pid)
{
    char **argv = NULL;
    char * dir_cname = NULL;
    pid_t loc_pid = 0;
    int status;
    bool is_tar = false;

    if( 0 == strncmp(&(cname[strlen(cname)-8]), ".tar.bz2", strlen(".tar.bz2")) ) {
        is_tar = true;
    }

    *fname = strdup(cname);
    if( is_tar ) {
        (*fname)[strlen(cname)-8] = '\0';
    } else {
        (*fname)[strlen(cname)-4] = '\0';
    }

    pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                        "compress:bzip: decompress_nb(%s -> [%s])",
                        cname, *fname);

    *child_pid = fork();
    if( *child_pid == 0 ) { /* Child */
        dir_cname  = pmix_dirname(cname);

        chdir(dir_cname);

        /* Fork(bunzip) */
        loc_pid = fork();
        if( loc_pid == 0 ) { /* Child */
            char * cmd;
            pmix_asprintf(&cmd, "bunzip2 %s", cname);

            pmix_output_verbose(10, mca_compress_bzip_component.super.output_handle,
                                "compress:bzip: decompress_nb() command [%s]",
                                cmd);

            argv = pmix_argv_split(cmd, ' ');
            status = execvp(argv[0], argv);

            pmix_output(0, "compress:bzip: decompress_nb: Failed to exec child [%s] status = %d\n", cmd, status);
            exit(PMIX_ERROR);
        }
        else if( loc_pid > 0 ) { /* Parent */
            waitpid(loc_pid, &status, 0);
            if( !WIFEXITED(status) ) {
                pmix_output(0, "compress:bzip: decompress_nb: Failed to bunzip the file [%s] status = %d\n", cname, status);
                exit(PMIX_ERROR);
            }
        }
        else {
            exit(PMIX_ERROR);
        }

        /* tar_decompress */
        if( is_tar ) {
            /* Strip off '.bz2' leaving just '.tar' */
            cname[strlen(cname)-4] = '\0';
            pmix_compress_base_tar_extract(&cname);
        }

        /* Once this child is done, then directly exit */
        exit(PMIX_SUCCESS);
    }
    else if( *child_pid > 0 ) {
        ;
    }
    else {
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

static bool is_directory(char *fname ) {
    struct stat file_status;
    int rc;

    if(0 != (rc = stat(fname, &file_status) ) ) {
        return false;
    }
    if(S_ISDIR(file_status.st_mode)) {
        return true;
    }

    return false;
}
