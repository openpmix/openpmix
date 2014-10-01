/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Buffer management types.
 */

#ifndef PMIX_BFROP_TYPES_H_
#define PMIX_BFROP_TYPES_H_

#include "pmix_config.h"
#include "src/include/types.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/pmix_object.h"
#include "src/class/pmix_pointer_array.h"
#include "src/class/pmix_list.h"

BEGIN_C_DECLS

typedef uint8_t pmix_data_type_t;  /** data type indicators */
#define PMIX_DATA_TYPE_T    PMIX_UINT8
#define PMIX_BFROP_ID_MAX     UINT8_MAX
#define PMIX_BFROP_ID_INVALID PMIX_BFROP_ID_MAX

/* define a structure to hold generic byte objects */
typedef struct {
    int32_t size;
    uint8_t *bytes;
} pmix_byte_object_t;

/* Type defines for packing and unpacking */
#define    PMIX_UNDEF               (pmix_data_type_t)    0 /**< type hasn't been defined yet */
#define    PMIX_BYTE                (pmix_data_type_t)    1 /**< a byte of data */
#define    PMIX_BOOL                (pmix_data_type_t)    2 /**< boolean */
#define    PMIX_STRING              (pmix_data_type_t)    3 /**< a NULL terminated string */
#define    PMIX_SIZE                (pmix_data_type_t)    4 /**< the generic size_t */
#define    PMIX_PID                 (pmix_data_type_t)    5 /**< process pid */
    /* all the integer flavors */
#define    PMIX_INT                 (pmix_data_type_t)    6 /**< generic integer */
#define    PMIX_INT8                (pmix_data_type_t)    7 /**< an 8-bit integer */
#define    PMIX_INT16               (pmix_data_type_t)    8 /**< a 16-bit integer */
#define    PMIX_INT32               (pmix_data_type_t)    9 /**< a 32-bit integer */
#define    PMIX_INT64               (pmix_data_type_t)   10 /**< a 64-bit integer */
    /* all the unsigned integer flavors */
#define    PMIX_UINT                (pmix_data_type_t)   11 /**< generic unsigned integer */
#define    PMIX_UINT8               (pmix_data_type_t)   12 /**< an 8-bit unsigned integer */
#define    PMIX_UINT16              (pmix_data_type_t)   13 /**< a 16-bit unsigned integer */
#define    PMIX_UINT32              (pmix_data_type_t)   14 /**< a 32-bit unsigned integer */
#define    PMIX_UINT64              (pmix_data_type_t)   15 /**< a 64-bit unsigned integer */
    /* floating point types */
#define    PMIX_FLOAT               (pmix_data_type_t)   16
#define    PMIX_DOUBLE              (pmix_data_type_t)   17
    /* system types */
#define    PMIX_TIMEVAL             (pmix_data_type_t)   18
#define    PMIX_TIME                (pmix_data_type_t)   19
    /* PMIX types */
#define    PMIX_BYTE_OBJECT         (pmix_data_type_t)   20 /**< byte object structure */
#define    PMIX_DATA_TYPE           (pmix_data_type_t)   21 /**< data type */
#define    PMIX_NULL                (pmix_data_type_t)   22 /**< don't interpret data type */
#define    PMIX_PSTAT               (pmix_data_type_t)   23 /**< process statistics */
#define    PMIX_NODE_STAT           (pmix_data_type_t)   24 /**< node statistics */
#define    PMIX_HWLOC_TOPO          (pmix_data_type_t)   25 /**< hwloc topology */
#define    PMIX_VALUE               (pmix_data_type_t)   26 /**< pmix value structure */
#define    PMIX_BUFFER              (pmix_data_type_t)   27 /**< pack the remaining contents of a buffer as an object */
#define    PMIX_PTR                 (pmix_data_type_t)   28 /**< pointer to void* */
    /* PMIX Dynamic */
#define    PMIX_BFROP_ID_DYNAMIC      (pmix_data_type_t)   30
    /* PMIX Array types */
#define    PMIX_FLOAT_ARRAY         (pmix_data_type_t)   31
#define    PMIX_DOUBLE_ARRAY        (pmix_data_type_t)   32
#define    PMIX_STRING_ARRAY        (pmix_data_type_t)   33
#define    PMIX_BOOL_ARRAY          (pmix_data_type_t)   34
#define    PMIX_SIZE_ARRAY          (pmix_data_type_t)   35
#define    PMIX_BYTE_ARRAY          (pmix_data_type_t)   36
#define    PMIX_INT_ARRAY           (pmix_data_type_t)   37
#define    PMIX_INT8_ARRAY          (pmix_data_type_t)   38
#define    PMIX_INT16_ARRAY         (pmix_data_type_t)   39
#define    PMIX_INT32_ARRAY         (pmix_data_type_t)   40
#define    PMIX_INT64_ARRAY         (pmix_data_type_t)   41
#define    PMIX_UINT_ARRAY          (pmix_data_type_t)   42
#define    PMIX_UINT8_ARRAY         (pmix_data_type_t)   43
#define    PMIX_UINT16_ARRAY        (pmix_data_type_t)   44
#define    PMIX_UINT32_ARRAY        (pmix_data_type_t)   45
#define    PMIX_UINT64_ARRAY        (pmix_data_type_t)   46
#define    PMIX_BYTE_OBJECT_ARRAY   (pmix_data_type_t)   47
#define    PMIX_PID_ARRAY           (pmix_data_type_t)   48
#define    PMIX_TIMEVAL_ARRAY       (pmix_data_type_t)   49


/* define the results values for comparisons so we can change them in only one place */
#define PMIX_VALUE1_GREATER  +1
#define PMIX_VALUE2_GREATER  -1
#define PMIX_EQUAL            0

/* List types for pmix_value_t, needs number of elements and a pointer */
/* float array object */
typedef struct {
    int32_t size;
    float *data;
} pmix_float_array_t;
/* double array object */
typedef struct {
    int32_t size;
    double *data;
} pmix_double_array_t;
/* string array object */
typedef struct {
    int32_t size;
    char **data;
} pmix_string_array_t;
/* bool array object */
typedef struct {
    int32_t size;
    bool *data;
} pmix_bool_array_t;
/* size array object */
typedef struct {
    int32_t size;
    size_t *data;
} pmix_size_array_t;
/* pmix byte object array object */
typedef struct {
    int32_t size;
    pmix_byte_object_t *data;
} pmix_byte_object_array_t;
/* int array object */
typedef struct {
    int32_t size;
    int *data;
} pmix_int_array_t;
/* int8 array object */
typedef struct {
    int32_t size;
    int8_t *data;
} pmix_int8_array_t;
/* int16 array object */
typedef struct {
    int32_t size;
    int16_t *data;
} pmix_int16_array_t;
/* int32 array object */
typedef struct {
    int32_t size;
    int32_t *data;
} pmix_int32_array_t;
/* int64 array object */
typedef struct {
    int32_t size;
    int64_t *data;
} pmix_int64_array_t;
/* uint array object */
typedef struct {
    int32_t size;
    unsigned int *data;
} pmix_uint_array_t;
/* uint8 array object */
typedef struct {
    int32_t size;
    uint8_t *data;
} pmix_uint8_array_t;
/* uint16 array object */
typedef struct {
    int32_t size;
    uint16_t *data;
} pmix_uint16_array_t;
/* uint32 array object */
typedef struct {
    int32_t size;
    uint32_t *data;
} pmix_uint32_array_t;
/* uint64 array object */
typedef struct {
    int32_t size;
    uint64_t *data;
} pmix_uint64_array_t;
/* pid array object */
typedef struct {
    int32_t size;
    pid_t *data;
} pmix_pid_array_t;
/* timeval array object */
typedef struct {
    int32_t size;
    struct timeval *data;
} pmix_timeval_array_t;

/* Data value object */
typedef struct {
    pmix_list_item_t super;             /* required for this to be on lists */
    char *key;                          /* key string */
    pmix_data_type_t type;              /* the type of value stored */
    union {
        bool flag;
        uint8_t byte;
        char *string;
        size_t size;
        pid_t pid;
        int integer;
        int8_t int8;
        int16_t int16;
        int32_t int32;
        int64_t int64;
        unsigned int uint;
        uint8_t uint8;
        uint16_t uint16;
        uint32_t uint32;
        uint64_t uint64;
        pmix_byte_object_t bo;
        float fval;
        double dval;
        struct timeval tv;
        pmix_bool_array_t flag_array;
        pmix_uint8_array_t byte_array;
        pmix_string_array_t string_array;
        pmix_size_array_t size_array;
        pmix_int_array_t integer_array;
        pmix_int8_array_t int8_array;
        pmix_int16_array_t int16_array;
        pmix_int32_array_t int32_array;
        pmix_int64_array_t int64_array;
        pmix_uint_array_t uint_array;
        pmix_uint8_array_t uint8_array;
        pmix_uint16_array_t uint16_array;
        pmix_uint32_array_t uint32_array;
        pmix_uint64_array_t uint64_array;
        pmix_byte_object_array_t bo_array;
        pmix_float_array_t fval_array;
        pmix_double_array_t dval_array;
        pmix_pid_array_t pid_array;
        pmix_timeval_array_t tv_array;
        void *ptr;  // never packed or passed anywhere
    } data;
} pmix_value_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_value_t);

/* Process statistics object */
#define PMIX_PSTAT_MAX_STRING_LEN   32
typedef struct {
    pmix_list_item_t super;                /* required for this to be on a list */
    /* process ident info */
    char node[PMIX_PSTAT_MAX_STRING_LEN];
    int32_t rank;
    pid_t pid;
    char cmd[PMIX_PSTAT_MAX_STRING_LEN];
    /* process stats */
    char state[2];
    struct timeval time;
    float percent_cpu;
    int32_t priority;
    int16_t num_threads;
    float vsize;  /* in MBytes */
    float rss;  /* in MBytes */
    float peak_vsize;  /* in MBytes */
    int16_t processor;
    /* time at which sample was taken */
    struct timeval sample_time;
} pmix_pstats_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_pstats_t);
typedef struct {
    pmix_list_item_t super;
    char *disk;
    unsigned long num_reads_completed;
    unsigned long num_reads_merged;
    unsigned long num_sectors_read;
    unsigned long milliseconds_reading;
    unsigned long num_writes_completed;
    unsigned long num_writes_merged;
    unsigned long num_sectors_written;
    unsigned long milliseconds_writing;
    unsigned long num_ios_in_progress;
    unsigned long milliseconds_io;
    unsigned long weighted_milliseconds_io;
} pmix_diskstats_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_diskstats_t);
typedef struct {
    pmix_list_item_t super;
    char *net_interface;
    unsigned long num_bytes_recvd;
    unsigned long num_packets_recvd;
    unsigned long num_recv_errs;
    unsigned long num_bytes_sent;
    unsigned long num_packets_sent;
    unsigned long num_send_errs;
} pmix_netstats_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_netstats_t);
typedef struct {
    pmix_object_t super;
    /* node-level load averages */
    float la;
    float la5;
    float la15;
    /* memory usage */
    float total_mem;  /* in MBytes */
    float free_mem;  /* in MBytes */
    float buffers;  /* in MBytes */
    float cached;   /* in MBytes */
    float swap_cached;  /* in MBytes */
    float swap_total;   /* in MBytes */
    float swap_free;    /* in MBytes */
    float mapped;       /* in MBytes */
    /* time at which sample was taken */
    struct timeval sample_time;
    /* list of disk stats, one per disk */
    pmix_list_t diskstats;
    /* list of net stats, one per interface */
    pmix_list_t netstats;

} pmix_node_stats_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_node_stats_t);

/* structured-unstructured data flags */
#define PMIX_BFROP_STRUCTURED     true
#define PMIX_BFROP_UNSTRUCTURED   false

/**
 * buffer type
 */
enum pmix_bfrop_buffer_type_t {
    PMIX_BFROP_BUFFER_NON_DESC   = 0x00,
    PMIX_BFROP_BUFFER_FULLY_DESC = 0x01
};

typedef enum pmix_bfrop_buffer_type_t pmix_bfrop_buffer_type_t;

#define PMIX_BFROP_BUFFER_TYPE_HTON(h);
#define PMIX_BFROP_BUFFER_TYPE_NTOH(h);

/**
 * Structure for holding a buffer to be used with the RML or OOB
 * subsystems.
 */
struct pmix_buffer_t {
    /** First member must be the object's parent */
    pmix_object_t parent;
    /** type of buffer */
    pmix_bfrop_buffer_type_t type;
    /** Start of my memory */
    char *base_ptr;
    /** Where the next data will be packed to (within the allocated
        memory starting at base_ptr) */
    char *pack_ptr;
    /** Where the next data will be unpacked from (within the
        allocated memory starting as base_ptr) */
    char *unpack_ptr;
    
    /** Number of bytes allocated (starting at base_ptr) */
    size_t bytes_allocated;
    /** Number of bytes used by the buffer (i.e., amount of data --
        including overhead -- packed in the buffer) */
    size_t bytes_used;
};
/**
 * Convenience typedef
 */
typedef struct pmix_buffer_t pmix_buffer_t;

/** formalize the declaration */
PMIX_DECLSPEC OBJ_CLASS_DECLARATION (pmix_buffer_t);

END_C_DECLS

#endif /* PMIX_BFROP_TYPES_H */
