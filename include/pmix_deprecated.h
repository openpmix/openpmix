/*
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer listed
 *   in this license in the documentation and/or other materials
 *   provided with the distribution.
 *
 * - Neither the name of the copyright holders nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * The copyright holders provide no reassurances that the source code
 * provided does not infringe any patent, copyright, or any other
 * intellectual property rights of third parties.  The copyright holders
 * disclaim any liability to any recipient for claims brought against
 * recipient by any third party for infringement of that parties
 * intellectual property rights.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $HEADER$
 *
 * PMIx provides a "function-shipping" approach to support for
 * implementing the server-side of the protocol. This method allows
 * resource managers to implement the server without being burdened
 * with PMIx internal details. Accordingly, each PMIx API is mirrored
 * here in a function call to be provided by the server. When a
 * request is received from the client, the corresponding server function
 * will be called with the information.
 *
 * Any functions not supported by the RM can be indicated by a NULL for
 * the function pointer. Client calls to such functions will have a
 * "not supported" error returned.
 */

#ifndef PMIx_DEPRECATED_H
#define PMIx_DEPRECATED_H

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/***** v4 DECPRECATIONS/REMOVALS *****/
/* APIs */
PMIX_EXPORT pmix_status_t PMIx_tool_connect_to_server(pmix_proc_t *proc,
                                                      pmix_info_t info[], size_t ninfo);

/* DATATYPES */
#define PMIX_BUFFER                     26

/* CONSTANTS */
#define PMIX_ERR_SILENT                             -2
#define PMIX_ERR_DEBUGGER_RELEASE                   -3
#define PMIX_ERR_PROC_ABORTED                       -7
#define PMIX_ERR_PROC_REQUESTED_ABORT               -8
#define PMIX_ERR_PROC_ABORTING                      -9
#define PMIX_ERR_SERVER_FAILED_REQUEST              -10
#define PMIX_EXISTS                                 -11
#define PMIX_ERR_HANDSHAKE_FAILED                   -13
#define PMIX_ERR_READY_FOR_HANDSHAKE                -14
#define PMIX_ERR_PROC_ENTRY_NOT_FOUND               -17
#define PMIX_ERR_PACK_MISMATCH                      -22
#define PMIX_ERR_IN_ERRNO                           -26
#define PMIX_ERR_DATA_VALUE_NOT_FOUND               -30
#define PMIX_ERR_INVALID_ARG                        -33
#define PMIX_ERR_INVALID_KEY                        -34
#define PMIX_ERR_INVALID_KEY_LENGTH                 -35
#define PMIX_ERR_INVALID_VAL                        -36
#define PMIX_ERR_INVALID_VAL_LENGTH                 -37
#define PMIX_ERR_INVALID_LENGTH                     -38
#define PMIX_ERR_INVALID_NUM_ARGS                   -39
#define PMIX_ERR_INVALID_ARGS                       -40
#define PMIX_ERR_INVALID_NUM_PARSED                 -41
#define PMIX_ERR_INVALID_KEYVALP                    -42
#define PMIX_ERR_INVALID_SIZE                       -43
#define PMIX_ERR_INVALID_NAMESPACE                  -44
#define PMIX_ERR_SERVER_NOT_AVAIL                   -45
#define PMIX_ERR_NOT_IMPLEMENTED                    -48
#define PMIX_DEBUG_WAITING_FOR_NOTIFY               -58
#define PMIX_ERR_LOST_CONNECTION_TO_SERVER          -101
#define PMIX_ERR_LOST_PEER_CONNECTION               -102
#define PMIX_ERR_LOST_CONNECTION_TO_CLIENT          -103
#define PMIX_NOTIFY_ALLOC_COMPLETE                  -105
#define PMIX_ERR_INVALID_TERMINATION                -112
#define PMIX_ERR_JOB_TERMINATED                     -145    // DEPRECATED NAME - non-error termination
#define PMIX_ERR_UPDATE_ENDPOINTS                   -146
#define PMIX_GDS_ACTION_COMPLETE                    -148
#define PMIX_PROC_HAS_CONNECTED                     -149
#define PMIX_CONNECT_REQUESTED                      -150
#define PMIX_ERR_NODE_DOWN                          -231
#define PMIX_ERR_NODE_OFFLINE                       -232

#define PMIX_ERR_SYS_BASE                           PMIX_EVENT_SYS_BASE
#define PMIX_ERR_SYS_OTHER                          PMIX_EVENT_SYS_OTHER

#define PMIX_JOB_STATE_PREPPED                   1  // job is ready to be launched

/* ATTRIBUTES */
#define PMIX_EVENT_BASE                     "pmix.evbase"           // (struct event_base *) pointer to libevent event_base
                                                                    // to use in place of the internal progress thread ***** DEPRECATED *****
#define PMIX_TOPOLOGY                       "pmix.topo"             // (hwloc_topology_t) ***** DEPRECATED ***** pointer to the PMIx client's internal
                                                                    //         topology object
#define PMIX_DEBUG_JOB                      "pmix.dbg.job"          // (char*) ***** DEPRECATED ***** nspace of the job assigned to this debugger to be debugged. Note
                                                                    //         that id's, pids, and other info on the procs is available
                                                                    //         via a query for the nspace's local or global proctable
#define PMIX_RECONNECT_SERVER               "pmix.cnct.recon"       // (bool) tool is requesting to change server connections
#define PMIX_ALLOC_NETWORK                  "pmix.alloc.net"        // (pmix_data_array_t*) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_ID               "pmix.alloc.netid"      // (char*) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_QOS              "pmix.alloc.netqos"     // (char*) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_TYPE             "pmix.alloc.nettype"    // (char*) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_PLANE            "pmix.alloc.netplane"   // (char*) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_ENDPTS           "pmix.alloc.endpts"     // (size_t) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_ENDPTS_NODE      "pmix.alloc.endpts.nd"  // (size_t) ***** DEPRECATED *****
#define PMIX_ALLOC_NETWORK_SEC_KEY          "pmix.alloc.nsec"       // (pmix_byte_object_t) ***** DEPRECATED *****
#define PMIX_PROC_DATA                      "pmix.pdata"            // (pmix_data_array_t*) ***** DEPRECATED ***** starts with rank, then contains more data
#define PMIX_LOCALITY                       "pmix.loc"              // (uint16_t)  ***** DEPRECATED *****relative locality of two procs

#define PMIX_LOCAL_TOPO                     "pmix.ltopo"            // (char*)  ***** DEPRECATED *****xml-representation of local node topology
#define PMIX_TOPOLOGY_XML                   "pmix.topo.xml"         // (char*)  ***** DEPRECATED *****XML-based description of topology
#define PMIX_TOPOLOGY_FILE                  "pmix.topo.file"        // (char*)  ***** DEPRECATED *****full path to file containing XML topology description
#define PMIX_TOPOLOGY_SIGNATURE             "pmix.toposig"          // (char*)  ***** DEPRECATED *****topology signature string
#define PMIX_HWLOC_SHMEM_ADDR               "pmix.hwlocaddr"        // (size_t)  ***** DEPRECATED *****address of HWLOC shared memory segment
#define PMIX_HWLOC_SHMEM_SIZE               "pmix.hwlocsize"        // (size_t)  ***** DEPRECATED *****size of HWLOC shared memory segment
#define PMIX_HWLOC_SHMEM_FILE               "pmix.hwlocfile"        // (char*)  ***** DEPRECATED *****path to HWLOC shared memory file
#define PMIX_HWLOC_XML_V1                   "pmix.hwlocxml1"        // (char*)  ***** DEPRECATED ***** XML representation of local topology using HWLOC v1.x format
#define PMIX_HWLOC_XML_V2                   "pmix.hwlocxml2"        // (char*) ***** DEPRECATED ***** XML representation of local topology using HWLOC v2.x format
#define PMIX_HWLOC_SHARE_TOPO               "pmix.hwlocsh"          // (bool) ***** DEPRECATED ***** Share the HWLOC topology via shared memory
#define PMIX_HWLOC_HOLE_KIND                "pmix.hwlocholek"       // (char*) ***** DEPRECATED ***** Kind of VM "hole" HWLOC should use for shared memory
#define PMIX_DSTPATH                        "pmix.dstpath"          // (char*) ***** DEPRECATED ***** path to dstore files
#define PMIX_COLLECTIVE_ALGO                "pmix.calgo"            // (char*) ***** DEPRECATED ***** comma-delimited list of algorithms to use for collective
#define PMIX_COLLECTIVE_ALGO_REQD           "pmix.calreqd"          // (bool) ***** DEPRECATED ***** if true, indicates that the requested choice of algo is mandatory
#define PMIX_PROC_BLOB                      "pmix.pblob"            // (pmix_byte_object_t) ***** DEPRECATED ***** packed blob of process data
#define PMIX_MAP_BLOB                       "pmix.mblob"            // (pmix_byte_object_t) ***** DEPRECATED ***** packed blob of process location
#define PMIX_MAPPER                         "pmix.mapper"           // (char*) ***** DEPRECATED ***** mapper to use for placing spawned procs
#define PMIX_NON_PMI                        "pmix.nonpmi"           // (bool) ***** DEPRECATED ***** spawned procs will not call PMIx_Init
#define PMIX_PROC_URI                       "pmix.puri"             // (char*) ***** DEPRECATED ***** URI containing contact info for proc
#define PMIX_ARCH                           "pmix.arch"             // (uint32_t) ***** DEPRECATED ***** datatype architecture flag

#define PMIX_DEBUG_JOB_DIRECTIVES           "pmix.dbg.jdirs"        // (pmix_data_array_t*) ***** DEPRECATED ***** array of job-level directives
#define PMIX_DEBUG_APP_DIRECTIVES           "pmix.dbg.adirs"        // (pmix_data_array_t*) ***** DEPRECATED ***** array of app-level directives
#define PMIX_EVENT_NO_TERMINATION           "pmix.evnoterm"         // (bool) ***** DEPRECATED ***** indicates that the handler has satisfactorily handled
                                                                    //        the event and believes termination of the application is not required
#define PMIX_EVENT_WANT_TERMINATION         "pmix.evterm"           // (bool) ***** DEPRECATED ***** indicates that the handler has determined that the
                                                                    //        application should be terminated
#define PMIX_TAG_OUTPUT                     "pmix.tagout"           // (bool) ***** DEPRECATED ***** tag application output with the ID of the source
#define PMIX_TIMESTAMP_OUTPUT               "pmix.tsout"            // (bool) ***** DEPRECATED ***** timestamp output from applications
#define PMIX_MERGE_STDERR_STDOUT            "pmix.mergeerrout"      // (bool) ***** DEPRECATED ***** merge stdout and stderr streams from application procs
#define PMIX_OUTPUT_TO_FILE                 "pmix.outfile"          // (char*) ***** DEPRECATED ***** direct application output into files of form
                                                                    //         "<filename>.rank" with both stdout and stderr redirected into it
#define PMIX_OUTPUT_TO_DIRECTORY            "pmix.outdir"           // (char*) ***** DEPRECATED ***** direct application output into files of form
                                                                    //         "<directory>/<jobid>/rank.<rank>/stdout[err]"
#define PMIX_OUTPUT_NOCOPY                  "pmix.nocopy"           // (bool) ***** DEPRECATED ***** output only into designated files - do not also output
                                                                    //        a copy to stdout/stderr

/* attributes for GDS */
#define PMIX_GDS_MODULE                     "pmix.gds.mod"          // (char*) ***** DEPRECATED ***** comma-delimited string of desired modules


#define PMIX_IOF_STOP                       "pmix.iof.stop"         // (bool) ***** DEPRECATED ***** Stop forwarding the specified channel(s)
#define PMIX_NOTIFY_LAUNCH                  "pmix.note.lnch"        // (bool) ***** DEPRECATED ***** notify the requestor upon launch of the child job and return
                                                                    //        its namespace in the event

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif
