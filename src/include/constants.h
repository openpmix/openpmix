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
 * Copyright (c) 2010-2012 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_CONSTANTS_H
#define PMIX_CONSTANTS_H
 
/* error codes - don't forget to update pmix/rutime/pmix_init.c when 
   adding to this list */
#define PMIX_ERR_BASE             0 /* internal use only */
 
enum {
    PMIX_SUCCESS                            = (PMIX_ERR_BASE),

    PMIX_ERROR                              = (PMIX_ERR_BASE -  1),
    PMIX_ERR_OUT_OF_RESOURCE                = (PMIX_ERR_BASE -  2), /* fatal error */
    PMIX_ERR_TEMP_OUT_OF_RESOURCE           = (PMIX_ERR_BASE -  3), /* try again later */
    PMIX_ERR_RESOURCE_BUSY                  = (PMIX_ERR_BASE -  4),
    PMIX_ERR_BAD_PARAM                      = (PMIX_ERR_BASE -  5),  /* equivalent to MPI_ERR_ARG error code */
    PMIX_ERR_FATAL                          = (PMIX_ERR_BASE -  6),
    PMIX_ERR_NOT_IMPLEMENTED                = (PMIX_ERR_BASE -  7),
    PMIX_ERR_NOT_SUPPORTED                  = (PMIX_ERR_BASE -  8),
    PMIX_ERR_INTERUPTED                     = (PMIX_ERR_BASE -  9),
    PMIX_ERR_WOULD_BLOCK                    = (PMIX_ERR_BASE - 10),
    PMIX_ERR_IN_ERRNO                       = (PMIX_ERR_BASE - 11),
    PMIX_ERR_UNREACH                        = (PMIX_ERR_BASE - 12),
    PMIX_ERR_NOT_FOUND                      = (PMIX_ERR_BASE - 13),
    PMIX_EXISTS                             = (PMIX_ERR_BASE - 14), /* indicates that the specified object already exists */
    PMIX_ERR_TIMEOUT                        = (PMIX_ERR_BASE - 15),
    PMIX_ERR_NOT_AVAILABLE                  = (PMIX_ERR_BASE - 16),
    PMIX_ERR_PERM                           = (PMIX_ERR_BASE - 17), /* no permission */
    PMIX_ERR_VALUE_OUT_OF_BOUNDS            = (PMIX_ERR_BASE - 18),
    PMIX_ERR_FILE_READ_FAILURE              = (PMIX_ERR_BASE - 19),
    PMIX_ERR_FILE_WRITE_FAILURE             = (PMIX_ERR_BASE - 20),
    PMIX_ERR_FILE_OPEN_FAILURE              = (PMIX_ERR_BASE - 21),
    PMIX_ERR_PACK_MISMATCH                  = (PMIX_ERR_BASE - 22),
    PMIX_ERR_PACK_FAILURE                   = (PMIX_ERR_BASE - 23),
    PMIX_ERR_UNPACK_FAILURE                 = (PMIX_ERR_BASE - 24),
    PMIX_ERR_UNPACK_INADEQUATE_SPACE        = (PMIX_ERR_BASE - 25),
    PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER = (PMIX_ERR_BASE - 26),
    PMIX_ERR_TYPE_MISMATCH                  = (PMIX_ERR_BASE - 27),
    PMIX_ERR_OPERATION_UNSUPPORTED          = (PMIX_ERR_BASE - 28),
    PMIX_ERR_UNKNOWN_DATA_TYPE              = (PMIX_ERR_BASE - 29),
    PMIX_ERR_BUFFER                         = (PMIX_ERR_BASE - 30),
    PMIX_ERR_DATA_TYPE_REDEF                = (PMIX_ERR_BASE - 31),
    PMIX_ERR_DATA_OVERWRITE_ATTEMPT         = (PMIX_ERR_BASE - 32),
    PMIX_ERR_MODULE_NOT_FOUND               = (PMIX_ERR_BASE - 33),
    PMIX_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED   = (PMIX_ERR_BASE - 34),
    PMIX_ERR_TOPO_SOCKET_NOT_SUPPORTED      = (PMIX_ERR_BASE - 35),
    PMIX_ERR_TOPO_CORE_NOT_SUPPORTED        = (PMIX_ERR_BASE - 36),
    PMIX_ERR_NOT_ENOUGH_SOCKETS             = (PMIX_ERR_BASE - 37),
    PMIX_ERR_NOT_ENOUGH_CORES               = (PMIX_ERR_BASE - 38),
    PMIX_ERR_INVALID_PHYS_CPU               = (PMIX_ERR_BASE - 39),
    PMIX_ERR_MULTIPLE_AFFINITIES            = (PMIX_ERR_BASE - 40),
    PMIX_ERR_SLOT_LIST_RANGE                = (PMIX_ERR_BASE - 41),
    PMIX_ERR_NETWORK_NOT_PARSEABLE          = (PMIX_ERR_BASE - 42),
    PMIX_ERR_SILENT                         = (PMIX_ERR_BASE - 43),
    PMIX_ERR_NOT_INITIALIZED                = (PMIX_ERR_BASE - 44),
    PMIX_ERR_NOT_BOUND                      = (PMIX_ERR_BASE - 45),
    PMIX_ERR_TAKE_NEXT_OPTION               = (PMIX_ERR_BASE - 46),
    PMIX_ERR_PROC_ENTRY_NOT_FOUND           = (PMIX_ERR_BASE - 47),
    PMIX_ERR_DATA_VALUE_NOT_FOUND           = (PMIX_ERR_BASE - 48)
};

#define PMIX_ERR_MAX                (PMIX_ERR_BASE - 100)

#endif /* PMIX_CONSTANTS_H */

