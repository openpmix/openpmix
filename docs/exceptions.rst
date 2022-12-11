Exceptions to the PMIx Standard
===============================

Exceptions to the base PMIx Standard are listed here for each OpenPMIx
release. These exceptions are not indicative of any intent to stray
from the Standard, but instead represent the difference between the
pace of development of the library versus the normal Standard's
process. Accordingly, it is expected that the exceptions listed below
will make their way into a future release of the PMIx Standard and
then be removed from the list of exceptions in some future OpenPMIx
release.

Extensions
----------

OpenPMIx |opmix_ver| is based on the PMIx |std_ver| Standard. In
addition to the *Extensions*, this release includes the conversion of
all support macros to PMIx function APIs |mdash| e.g., the
``PMIX_LOAD_PROCID`` macro is now the ``PMIx_Load_procid()``
function. The macro versions have been retained as deprecated (without
warnings) for backward compatibility.

APIs
^^^^

* Load a key:

  .. code-block:: c

     void PMIx_Load_key(pmix_key_t key, const char *src);

* Check a key:

  .. code-block:: c

     bool PMIx_Check_key(const char *key, const char *str);

* Check to see if a key is a "reserved" key:

  .. code-block:: c

     bool PMIx_Check_reserved_key(const char *key);

* Load a string into a pmix_nspace_t struct:

  .. code-block:: c

     void PMIx_Load_nspace(pmix_nspace_t nspace, const char *str);

* Check two ``nspace`` structs for equality:

  .. code-block:: c

     bool PMIx_Check_nspace(const char *key1, const char *key2);

* Check if a namespace is invalid:

  .. code-block:: c

     bool PMIx_Nspace_invalid(const char *nspace);

* Load a process ID struct:

  .. code-block:: c

     void PMIx_Load_procid(pmix_proc_t *p,
                           const char *ns,
                           pmix_rank_t rk);

* Transfer a process ID struct (non-destructive):

  .. code-block:: c

     void PMIx_Xfer_procid(pmix_proc_t *dst,
                           const pmix_proc_t *src);

* Check two proc IDs for equality:

  .. code-block:: c

     bool PMIx_Check_procid(const pmix_proc_t *a,
                            const pmix_proc_t *b);

* Check two ranks for equality:

  .. code-block:: c

     bool PMIx_Check_rank(pmix_rank_t a,
                          pmix_rank_t b);

* Check if proc ID is invalid:

  .. code-block:: c

     bool PMIx_Procid_invalid(const pmix_proc_t *p);

* Argv handling:

  .. code-block:: c

     int PMIx_Argv_count(char **a);
     pmix_status_t PMIx_Argv_append_nosize(char ***argv, const char *arg);
     pmix_status_t PMIx_Argv_prepend_nosize(char ***argv, const char *arg);
     pmix_status_t PMIx_Argv_append_unique_nosize(char ***argv, const char *arg);
     void PMIx_Argv_free(char **argv);
     char **PMIx_Argv_split_inter(const char *src_string,
                                  int delimiter,
                                  bool include_empty);
     char **PMIx_Argv_split_with_empty(const char *src_string, int delimiter);
     char **PMIx_Argv_split(const char *src_string, int delimiter);
     char *PMIx_Argv_join(char **argv, int delimiter);
     char **PMIx_Argv_copy(char **argv);

* Set environment variable:

  .. code-block:: c

     pmix_status_t PMIx_Setenv(const char *name,
                               const char *value,
                               bool overwrite,
                               char ***env);

* Initialize a value struct:

  .. code-block:: c

     void PMIx_Value_construct(pmix_value_t *val);

* Free memory stored inside a value struct:

  .. code-block:: c

     void PMIx_Value_destruct(pmix_value_t *val);

* Create and initialize an array of value structs:

  .. code-block:: c

     pmix_value_t* PMIx_Value_create(size_t n);

* Free memory stored inside an array of coord structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Value_free(pmix_value_t *v, size_t n);

* Check the given value struct to determine if it includes a boolean
  value (includes strings for ``true`` and ``false``, including
  abbreviations such as ``t`` or ``f``), and if so, then its value. A
  value type of ``PMIX_UNDEF`` is taken to imply a boolean ``true``.

  .. code-block:: c

     pmix_boolean_t PMIx_Value_true(const pmix_value_t *v);

* Load data into a ``pmix_value_t`` structure. The data can be of any
  PMIx data type |mdash| which means the load can be somewhat complex
  to implement (e.g., in the case of a ``pmix_data_array_t``). The
  data is *copied* into the value struct:

  .. code-block:: c

     pmix_status_t PMIx_Value_load(pmix_value_t *val,
                                   const void *data,
                                   pmix_data_type_t type);

* Unload data from a ``pmix_value_t`` structure:

  .. code-block:: c

     pmix_status_t PMIx_Value_unload(pmix_value_t *val,
                                     void **data,
                                     size_t *sz);

* Transfer data from one ``pmix_value_t`` to another.  This is actually
  executed as a *copy* operation, so the original data is not altered:

  .. code-block:: c

     pmix_status_t PMIx_Value_xfer(pmix_value_t *dest,
                                   const pmix_value_t *src);

* Compare the contents of two ``pmix_value_t`` structures:

  .. code-block:: c

     pmix_value_cmp_t PMIx_Value_compare(pmix_value_t *v1,
                                         pmix_value_t *v2);

* Destroy a data array object:

  .. code-block:: c

     void PMIx_Data_array_destruct(pmix_data_array_t *d);

* Initialize an info struct:

  .. code-block:: c

     void PMIx_Info_construct(pmix_info_t *p);

* Free memory stored inside an info struct:

  .. code-block:: c

     void PMIx_Info_destruct(pmix_info_t *p);

* Create and initialize an array of info structs:

  .. code-block:: c

     pmix_info_t* PMIx_Info_create(size_t n);

* Free memory stored inside an array of coord structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Info_free(pmix_info_t *p, size_t n);

* Check the given info struct to determine if it includes
  a boolean value (includes strings for ``true`` and ``false``,
  including abbreviations such as ``t`` or ``f``), and if so,
  then its value. A value type of ``PMIX_UNDEF`` is taken to imply
  a boolean ``true`` as the presence of the key defaults to
  indicating ``true``.

  .. code-block:: c

     pmix_boolean_t PMIx_Info_true(const pmix_info_t *p);

* Load key/value data into a ``pmix_info_t`` struct. Note that this
  effectively is a ``PMIX_LOAD_KEY`` operation to copy the key,
  followed by a ``PMIx_Value_load`` to *copy* the data into the
  ``pmix_value_t`` in the provided info struct:

  .. code-block:: c

     pmix_status_t PMIx_Info_load(pmix_info_t *info,
                                  const char *key,
                                  const void *data,
                                  pmix_data_type_t type);

* Transfer data from one ``pmix_info_t`` to another.  This is actually
  executed as a *copy* operation, so the original data is not altered:

  .. code-block:: c

     pmix_status_t PMIx_Info_xfer(pmix_info_t *dest,
                                  const pmix_info_t *src);

* Mark the info struct as required:

  .. code-block:: c

     void PMIx_Info_required(pmix_info_t *p);

* Mark the info struct as optional:

  .. code-block:: c

     void PMIx_Info_optional(pmix_info_t *p);

* Check if the info struct is required:

  .. code-block:: c

     bool PMIx_Info_is_required(const pmix_info_t *p);

* Check if the info struct is optional:

  .. code-block:: c

     bool PMIx_Info_is_optional(const pmix_info_t *p);

* Mark the info struct as processed:

  .. code-block:: c

     void PMIx_Info_processed(pmix_info_t *p);

* Check if the info struct has been processed:

  .. code-block:: c

     bool PMIx_Info_was_processed(const pmix_info_t *p);

* Mark the info struct as the end of an array:

  .. code-block:: c

     void PMIx_Info_set_end(pmix_info_t *p);

* Check if the info struct is the end of an array:

  .. code-block:: c

     bool PMIx_Info_is_end(const pmix_info_t *p);

* Mark the info as a qualifier:

  .. code-block:: c

     void PMIx_Info_qualifier(pmix_info_t *p);

* Check if the info struct is a qualifier:

  .. code-block:: c

     bool PMIx_Info_is_qualifier(const pmix_info_t *p);

* Mark the info struct as persistent |mdash| do *not* release its contents:

  .. code-block:: c

     void PMIx_Info_persistent(pmix_info_t *p);

* Check if the info struct is persistent:

  .. code-block:: c

     bool PMIx_Info_is_persistent(const pmix_info_t *p);


* Initialize a coord struct:

  .. code-block:: c

     void PMIx_Coord_construct(pmix_coord_t *m);

* Free memory stored inside a coord struct:

  .. code-block:: c

     void PMIx_Coord_destruct(pmix_coord_t *m);

* Create and initialize an array of coord structs:

  .. code-block:: c

     pmix_coord_t* PMIx_Coord_create(size_t dims,
                                     size_t number);

* Free memory stored inside an array of coord structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Coord_free(pmix_coord_t *m, size_t number);

* Initialize a topology struct:

  .. code-block:: c

     void PMIx_Topology_construct(pmix_topology_t *t);

* Free memory stored inside a topology struct:

  .. code-block:: c

     void PMIx_Topology_destruct(pmix_topology_t *topo);

* Create and initialize an array of topology structs:

  .. code-block:: c

     pmix_topology_t* PMIx_Topology_create(size_t n);

* Free memory stored inside an array of topology structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Topology_free(pmix_topology_t *t, size_t n);

* Initialize a cpuset struct:

  .. code-block:: c

     void PMIx_Cpuset_construct(pmix_cpuset_t *cpuset);

* Free memory stored inside a cpuset struct:

  .. code-block:: c

     void PMIx_Cpuset_destruct(pmix_cpuset_t *cpuset);

* Create and initialize an array of cpuset structs:

  .. code-block:: c

     pmix_cpuset_t* PMIx_Cpuset_create(size_t n);

* Free memory stored inside an array of cpuset structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Cpuset_free(pmix_cpuset_t *c, size_t n);

* Initialize a geometry struct:

  .. code-block:: c

     void PMIx_Geometry_construct(pmix_geometry_t *g);

* Free memory stored inside a cpuset struct:

  .. code-block:: c

     void PMIx_Geometry_destruct(pmix_geometry_t *g);

* Create and initialize an array of cpuset structs:

  .. code-block:: c

     pmix_geometry_t* PMIx_Geometry_create(size_t n);

* Free memory stored inside an array of cpuset structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Geometry_free(pmix_geometry_t *g, size_t n);

* Initialize a device distance struct:

  .. code-block:: c

     void PMIx_Device_distance_construct(pmix_device_distance_t *d);

* Free memory stored inside a device distance struct:

  .. code-block:: c

     void PMIx_Device_distance_destruct(pmix_device_distance_t *d);

* Create and initialize an array of device distance structs:

  .. code-block:: c

     pmix_device_distance_t* PMIx_Device_distance_create(size_t n);

* Free memory stored inside an array of device distance structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Device_distance_free(pmix_device_distance_t *d, size_t n);

* Initialize a byte object struct:

  .. code-block:: c

     void PMIx_Byte_object_construct(pmix_byte_object_t *b);

* Free memory stored inside a byte object struct:

  .. code-block:: c

     void PMIx_Byte_object_destruct(pmix_byte_object_t *g);

* Create and initialize an array of byte object structs:

  .. code-block:: c

     pmix_byte_object_t* PMIx_Byte_object_create(size_t n);

* Free memory stored inside an array of byte object structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Byte_object_free(pmix_byte_object_t *g, size_t n);

* Load a byte object:

  .. code-block:: c

     void PMIx_Byte_object_load(pmix_byte_object_t *b,
                                char *d, size_t sz);

* Initialize an endpoint struct:

  .. code-block:: c

     void PMIx_Endpoint_construct(pmix_endpoint_t *e);

* Free memory stored inside an endpoint struct:

  .. code-block:: c

     void PMIx_Endpoint_destruct(pmix_endpoint_t *e);

* Create and initialize an array of endpoint structs:

  .. code-block:: c

     pmix_endpoint_t* PMIx_Endpoint_create(size_t n);

* Free memory stored inside an array of endpoint structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Endpoint_free(pmix_endpoint_t *e, size_t n);

* Initialize an envar struct:

  .. code-block:: c

     void PMIx_Envar_construct(pmix_envar_t *e);

* Free memory stored inside an envar struct:

  .. code-block:: c

     void PMIx_Envar_destruct(pmix_envar_t *e);

* Create and initialize an array of envar structs:

  .. code-block:: c

     pmix_envar_t* PMIx_Envar_create(size_t n);

* Free memory stored inside an array of envar structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Envar_free(pmix_envar_t *e, size_t n);

* Load an envar struct:

  .. code-block:: c

     void PMIx_Envar_load(pmix_envar_t *e,
                          char *var,
                          char *value,
                          char separator);

* Initialize a data buffer struct:

  .. code-block:: c

     void PMIx_Data_buffer_construct(pmix_data_buffer_t *b);

* Free memory stored inside a data buffer struct:

  .. code-block:: c

     void PMIx_Data_buffer_destruct(pmix_data_buffer_t *b);

* Create a data buffer struct:

  .. code-block:: c

     pmix_data_buffer_t* PMIx_Data_buffer_create(void);

* Free memory stored inside a data buffer struct:

  .. code-block:: c

     void PMIx_Data_buffer_release(pmix_data_buffer_t *b);

* Load a data buffer struct:

  .. code-block:: c

     void PMIx_Data_buffer_load(pmix_data_buffer_t *b,
                                char *bytes, size_t sz);

* Unload a data buffer struct:

  .. code-block:: c

     void PMIx_Data_buffer_unload(pmix_data_buffer_t *b,
                                  char **bytes, size_t *sz);


* Initialize a proc struct:

  .. code-block:: c

     void PMIx_Proc_construct(pmix_proc_t *p);

* Clear memory inside a proc struct:

  .. code-block:: c

     void PMIx_Proc_destruct(pmix_proc_t *p);

* Create and initialize an array of proc structs:

  .. code-block:: c

     pmix_proc_t* PMIx_Proc_create(size_t n);

* Free memory stored inside an array of proc structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Proc_free(pmix_proc_t *p, size_t n);

* Load a proc struct:

  .. code-block:: c

     void PMIx_Proc_load(pmix_proc_t *p,
                         char *nspace, pmix_rank_t rank);

* Construct a multicluster ``nspace`` struct from cluster and
  ``nspace`` values:

  .. code-block:: c

     void PMIx_Multicluster_nspace_construct(pmix_nspace_t target,
                                             pmix_nspace_t cluster,
                                             pmix_nspace_t nspace);

* Parse a multicluster nspace struct to separate out the cluster
  and ``nspace`` portions:

  .. code-block:: c

     void PMIx_Multicluster_nspace_parse(pmix_nspace_t target,
                                         pmix_nspace_t cluster,
                                         pmix_nspace_t nspace);


* Initialize a proc info struct:

  .. code-block:: c

     void PMIx_Proc_info_construct(pmix_proc_info_t *p);

* Clear memory inside a proc info struct:

  .. code-block:: c

     void PMIx_Proc_info_destruct(pmix_proc_info_t *p);

* Create and initialize an array of proc info structs:

  .. code-block:: c

     pmix_proc_info_t* PMIx_Proc_info_create(size_t n);

* Free memory stored inside an array of proc info structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Proc_info_free(pmix_proc_info_t *p, size_t n);

* Initialize a proc stats struct:

  .. code-block:: c

     void PMIx_Proc_stats_construct(pmix_proc_stats_t *p);

* Clear memory inside a proc stats struct:

  .. code-block:: c

     void PMIx_Proc_stats_destruct(pmix_proc_stats_t *p);

* Create and initialize an array of proc stats structs:

  .. code-block:: c

     pmix_proc_stats_t* PMIx_Proc_stats_create(size_t n);

* Free memory stored inside an array of proc stats structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Proc_stats_free(pmix_proc_stats_t *p, size_t n);

* Initialize a disk stats struct:

  .. code-block:: c

     void PMIx_Disk_stats_construct(pmix_disk_stats_t *p);

* Clear memory inside a disk stats struct:

  .. code-block:: c

     void PMIx_Disk_stats_destruct(pmix_disk_stats_t *p);

* Create and initialize an array of disk stats structs:

  .. code-block:: c

     pmix_disk_stats_t* PMIx_Disk_stats_create(size_t n);

* Free memory stored inside an array of disk stats structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Disk_stats_free(pmix_disk_stats_t *p, size_t n);

* Initialize a net stats struct:

  .. code-block:: c

     void PMIx_Net_stats_construct(pmix_net_stats_t *p);

* Clear memory inside a net stats struct:

  .. code-block:: c

     void PMIx_Net_stats_destruct(pmix_net_stats_t *p);

* Create and initialize an array of net stats structs:

  .. code-block:: c

     pmix_net_stats_t* PMIx_Net_stats_create(size_t n);

* Free memory stored inside an array of net stats structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Net_stats_free(pmix_net_stats_t *p, size_t n);

* Initialize a pdata struct:

  .. code-block:: c

     void PMIx_Pdata_construct(pmix_pdata_t *p);

* Clear memory inside a pdata struct:

  .. code-block:: c

     void PMIx_Pdata_destruct(pmix_pdata_t *p);

* Create and initialize an array of pdata structs:

  .. code-block:: c

     pmix_pdata_t* PMIx_Pdata_create(size_t n);

* Free memory stored inside an array of pdata structs (does
  not free the struct memory itself):

  .. code-block:: c

     void PMIx_Pdata_free(pmix_pdata_t *p, size_t n);

* App operations:

  .. code-block:: c

     void PMIx_App_construct(pmix_app_t *p);
     void PMIx_App_destruct(pmix_app_t *p);
     pmix_app_t* PMIx_App_create(size_t n);
     void PMIx_App_info_create(pmix_app_t *p, size_t n);
     void PMIx_App_free(pmix_app_t *p, size_t n);
     void PMIx_App_release(pmix_app_t *p);

* Constructing arrays of ``pmix_info_t`` for passing to an API can
  be tedious since the ``pmix_info_t`` itself is not a "list object".
  Since this is a very frequent operation, a set of APIs has been
  provided that opaquely manipulates internal PMIx list structures
  for this purpose. The user only need provide a ``void*`` pointer to
  act as the caddy for the internal list object.

* Initialize a list of ``pmix_info_t`` structures:

  .. code-block:: c

     void* PMIx_Info_list_start(void);

* Add data to a list of ``pmix_info_t`` structs. The ``ptr`` passed
  here is the pointer returned by ``PMIx_Info_list_start``:

  .. code-block:: c

     pmix_status_t PMIx_Info_list_add(void *ptr,
                                      const char *key,
                                      const void *value,
                                      pmix_data_type_t type);
     pmix_status_t PMIx_Info_list_insert(void *ptr, pmix_info_t *info);

* Transfer the data in an existing ``pmix_info_t`` struct to a list. This
  is executed as a *copy* operation, so the original data is not altered.
  The ``ptr`` passed here is the pointer returned by ``PMIx_Info_list_start``:

  .. code-block:: c

     pmix_status_t PMIx_Info_list_xfer(void *ptr, const pmix_info_t *info);

* Convert the constructed list of ``pmix_info_t`` structs to a
  ``pmix_data_array_t`` of ``pmix_info_t``. Data on the list is
  *copied* to the array elements:

  .. code-block:: c

     pmix_status_t PMIx_Info_list_convert(void *ptr, pmix_data_array_t *par);

* Release all data on the list and destruct all internal tracking:

  .. code-block:: c

     void PMIx_Info_list_release(void *ptr);

* Check if the tool is connected to a PMIx server:

  .. code-block:: c

     bool PMIx_tool_is_connected(void);

* Request a session control action. The sessionID identifies the
  session to which the specified control action is to be applied. A
  ``UINT32_MAX`` value can be used to indicate all sessions under the
  caller's control.

  The directives are provided as ``pmix_info_t`` structs in the
  directives array. The callback function provides a status to
  indicate whether or not the request was granted, and to provide some
  information as to the reason for any denial in the
  ``pmix_info_cbfunc_t`` array of ``pmix_info_t`` structures. If
  non-NULL, then the specified release_fn must be called when the
  callback function completes |mdash| this will be used to release any
  provided ``pmix_info_t`` array.

  Passing NULL as the ``cbfunc`` to this call indicates that it shall
  be treated as a blocking operation, with the return status
  indicative of the overall operation's completion.

  .. code-block:: c

     pmix_status_t PMIx_Session_control(uint32_t sessionID,
                                        const pmix_info_t directives[], size_t ndirs,
                                        pmix_info_cbfunc_t cbfunc, void *cbdata);

* The following pretty-print support APIs have been added:

  .. code-block:: c

     const char* PMIx_Value_comparison_string(pmix_value_cmp_t cmp);
     char* PMIx_App_string(const pmix_app_t *app);

* The following pretty-print support APIs have been slightly modified
  to add a ``const`` qualifier to their input parameter:

  .. code-block:: c

     const char* PMIx_Get_attribute_string(const char *attribute);
     const char* PMIx_Get_attribute_name(const char *attrstring);
     char* PMIx_Info_string(const pmix_info_t *info);
     char* PMIx_Value_string(const pmix_value_t *value);

  This is not expected to cause any issues for users.

Constants
^^^^^^^^^

* ``PMIX_DATA_BUFFER``: data type for packing/unpacking of
  ``pmix_data_buffer_t`` objects
* ``PMIX_DISK_STATS``: data type for packing/unpacking of
  ``pmix_disk_stats_t`` objects
* ``PMIX_NET_STATS``: data type for packing/unpacking of
  ``pmix_net_stats_t`` objects
* ``PMIX_NODE_STATS``: data type for packing/unpacking of
  ``pmix_node_stats_t`` objects
* ``PMIX_PROC_STATS``: data type for packing/unpacking of
  ``pmix_proc_stats_t`` objects
* ``PMIX_ERR_JOB_EXE_NOT_FOUND``: specified executable not found
* ``PMIX_ERR_JOB_INSUFFICIENT_RESOURCES``: insufficient resources to
  spawn job
* ``PMIX_ERR_JOB_SYS_OP_FAILED``: system library operation failed
* ``PMIX_ERR_JOB_WDIR_NOT_FOUND``: specified working directory not
  found
* ``PMIX_READY_FOR_DEBUG``: event indicating job/proc is ready for
  debug (accompanied by ``PMIX_BREAKPOINT`` indicating where proc is
  waiting)
* ``PMIX_ERR_PROC_REQUESTED_ABORT``: process called ``PMIx_Abort``
* ``PMIX_ERR_PROC_KILLED_BY_CMD``: process was terminated by RTE
  command
* ``PMIX_ERR_PROC_FAILED_TO_START``: process failed to start
* ``PMIX_ERR_PROC_ABORTED_BY_SIG``: process aborted by signal (e.g.,
  segmentation fault)
* ``PMIX_ERR_PROC_SENSOR_BOUND_EXCEEDED``: process terminated due to
  exceeding a sensor boundary
* ``PMIX_ERR_EXIT_NONZERO_TERM``: process exited normally, but with a
  non-zero status


Attributes
^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_EXTERNAL_AUX_EVENT_BASE``
     - ``pmix.evaux``
     - ``(void*)``
     - event base to be used for auxiliary
       functions (e.g., capturing signals) that would
       otherwise interfere with the host
       
   * - ``PMIX_CONNECT_TO_SCHEDULER``
     - ``pmix.cnct.sched``
     - ``(bool)``
     - Connect to the system scheduler
       
   * - ``PMIX_BIND_PROGRESS_THREAD``
     - ``pmix.bind.pt``
     - ``(char*)``
     - Comma-delimited ranges of CPUs that the internal PMIx progress
       thread shall be bound to
         
   * - ``PMIX_BIND_REQUIRED``
     - ``pmix.bind.reqd``
     - ``(bool)``
     - Return error if the internal PMIx progress thread cannot be
       bound
           
   * - ``PMIX_COLOCATE_PROCS``
     - ``pmix.colproc``
     - ``(pmix_data_array_t*)``
     - Array of ``pmix_proc_t`` identifying the procs with which the
       new job's procs are to be colocated
       
   * - ``PMIX_COLOCATE_NPERPROC``
     - ``pmix.colnum.proc``
     - ``(uint16_t)``
     - Number of procs to colocate with each identified proc
       
   * - ``PMIX_COLOCATE_NPERNODE``
     - ``pmix.colnum.node``
     - ``(uint16_t)``
     - Number of procs to colocate on the node of each identified proc
       
   * - ``PMIX_EVENT_ONESHOT``
     - ``pmix.evone``
     - ``(bool)``
     - when registering, indicate that this event handler is to be
       deleted after being invoked

   * - ``PMIX_GROUP_ADD_MEMBERS``
     - ``pmix.grp.add``
     - ``(pmix_data_array_t*)``
     - Array of ``pmix_proc_t`` identifying procs that are not
       included in the membership specified in the procs array passed
       to the ``PMIx_Group_construct[_nb]()`` call, but are to be
       included in the final group. The identified procs will be sent
       an invitation to join the group during the construction
       procedure. This is used when some members of the proposed group
       do not know the full membership and therefore cannot include
       all members in the call to construct.
       
   * - ``PMIX_GROUP_LOCAL_CID``
     - ``pmix.grp.lclid``
     - ``(size_t)``
     - local context ID for the specified process member of a group
       
   * - ``PMIX_IOF_TAG_DETAILED_OUTPUT``
     - ``pmix.iof.tagdet``
     - ``(bool)``
     - Tag output with the [local jobid,rank][hostname:pid] and
       channel it comes from
       
   * - ``PMIX_IOF_TAG_FULLNAME_OUTPUT``
     - ``pmix.iof.tagfull``
     - ``(bool)``
     - Tag output with the [nspace,rank] and channel it comes from
       
   * - ``PMIX_LOG_AGG``
     - ``pmix.log.agg``
     - ``(bool)``
     - Whether to aggregate and prevent duplicate logging messages
         based on key value pairs.
         
   * - ``PMIX_LOG_KEY``
     - ``pmix.log.key``
     - ``(char*)``
     - key to a logging message
         
   * - ``PMIX_LOG_VAL``
     - ``pmix.log.val``
     - ``(char*)``
     - value to a logging message
         
   * - ``PMIX_MYSERVER_URI``
     - ``pmix.mysrvr.uri``
     - ``(char*)``
     - URI of this proc's listener socket
         
   * - ``PMIX_QUALIFIED_VALUE``
     - ``pmix.qual.val``
     - ``(pmix_data_array_t*)``
     - Value being provided consists of the primary
       key-value pair in first position, followed by one or more
       key-value qualifiers to be used when subsequently retrieving
       the primary value
         
   * - ``PMIX_WDIR_USER_SPECIFIED``
     - ``pmix.wdir.user``
     - ``(bool)``
     - User specified the working directory
         
   * - ``PMIX_RUNTIME_OPTIONS``
     - ``pmix.runopt``
     - ``(char*)``
     - Environment-specific runtime directives that control job
       behavior
         
   * - ``PMIX_ABORT_NON_ZERO_TERM``
     - ``pmix.abnz``
     - ``(bool)``
     - Abort the spawned job if any process terminates with non-zero
       status
         
   * - ``PMIX_DO_NOT_LAUNCH``
     - ``pmix.dnl``
     - ``(bool)``
     - Execute all procedures to prepare the requested job for
       launch, but do not launch it. Typically combined with the
       PMIX_DISPLAY_MAP or PMIX_DISPLAY_MAP_DETAILED for debugging
       purposes.
         
   * - ``PMIX_SHOW_LAUNCH_PROGRESS``
     - ``pmix.showprog``
     - ``(bool)``
     - Provide periodic progress reports on job launch procedure (e.g., after
       every 100 processes have been spawned)
         
   * - ``PMIX_AGGREGATE_HELP``
     - ``pmix.agg.help``
     - ``(bool)``
     - Aggregate help messages, reporting each unique help message once
       accompanied by the number of processes that reported it
         
   * - ``PMIX_REPORT_CHILD_SEP``
     - ``pmix.rptchildsep``
     - ``(bool)``
     - Report the exit status of any child jobs spawned by the
       primary job separately. If false, then the final exit status
       reported will be zero if the primary job and all spawned jobs
       exit normally, or the first non-zero status returned by
       either primary or child jobs.
         
   * - ``PMIX_DISPLAY_MAP_DETAILED``
     - ``pmix.dispmapdet``
     - ``(bool)``
     - display a highly detailed placement map upon spawn
       
   * - ``PMIX_DISPLAY_ALLOCATION``
     - ``pmix.dispalloc``
     - ``(bool)``
     - display the resource allocation
         
   * - ``PMIX_DISPLAY_TOPOLOGY``
     - ``pmix.disptopo``
     - ``(char*)``
     - comma-delimited list of hosts whose topology is
       to be displayed
         
   * - ``PMIX_SORTED_PROC_ARRAY``
     - ``pmix.sorted.parr``
     - ``(bool)``
     - Proc array being passed has been sorted
         
   * - ``PMIX_QUERY_ALLOCATION``
     - ``pmix.query.allc``
     - ``(pmix_data_array_t*)``
     - returns an array of ``pmix_info_t`` describing the nodes known to
       the server. Each array element will consist of the
       ``PMIX_NODE_INFO`` key containing a ``pmix_data_array_t`` of
       ``pmix_info_t`` |mdash| the first element of the array must be
       the hostname of that node, with additional info on the node in
       subsequent entries.  SUPPORTED_QUALIFIER: a ``PMIX_ALLOC_ID``
       qualifier indicating the specific allocation of interest
        
   * - ``PMIX_TOPOLOGY_INDEX``
     - ``pmix.topo.index``
     - ``(int)``
     - index of a topology in a storage array
         
   * - ``PMIX_ALLOC_PREEMPTIBLE``
     - ``pmix.alloc.preempt``
     - ``(bool)``
     - by default, all jobs in the resulting allocation are to be
       considered preemptible (overridable at per-job level)

.. note:: The attribute ``PMIX_DEBUG_STOP_IN_APP`` has been modified
          to only support a ``PMIX_BOOL`` value instead of an optional
          array of ranks due to questions over the use-case calling
          for stopping a subset of a job's processes while allowing
          others to run "free".

Session control attributes

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_SESSION_CTRL_ID``
     - ``pmix.ssnctrl.id``
     - ``(char*)``
     - provide a string identifier for this request

Session instantiation attributes |mdash| called by scheduler.
Schedulers calling to create a session are required to provide:

* the effective userID and groupID that the session should have
  when instantiated.
  
* description of the resources that are to be included in the session
  
* if applicable, the image that should be provisioned on nodes
  included in the session
  
* an array of applications (if any) that are to be started in the
  session once instantiated

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_SESSION_APP``
     - ``pmix.ssn.app``
     - ``(pmix_data_array_t*)``
     - Array of ``pmix_app_t`` to be executed in the assigned
       session upon session instantiation

   * - ``PMIX_SESSION_PROVISION``
     - ``pmix.ssn.pvn``
     - ``(pmix_data_array_t*)``
     - description of nodes to be provisioned with
       specified image
       
   * - ``PMIX_SESSION_PROVISION_NODES``
     - ``pmix.ssn.pvnnds``
     - ``(char*)``
     - regex identifying nodes that are to be provisioned
       
   * - ``PMIX_SESSION_PROVISION_IMAGE``
     - ``pmix.ssn.pvnimg``
     - ``(char*)``
     - name of the image that is to be provisioned

Session operational attributes.

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_SESSION_PAUSE``
     - ``pmix.ssn.pause``
     - ``(bool)``
     - pause all jobs in the specified session
       
   * - ``PMIX_SESSION_RESUME``
     - ``pmix.ssn.resume``
     - ``(bool)``
     - "un-pause" all jobs in the specified session
       
   * - ``PMIX_SESSION_TERMINATE``
     - ``pmix.ssn.terminate``
     - ``(bool)``
     - terminate all jobs in the specified session and recover all
       resources included in the session.
       
   * - ``PMIX_SESSION_PREEMPT``
     - ``pmix.ssn.preempt``
     - ``(bool)``
     - preempt indicated jobs (given in accompanying ``pmix_info_t`` via
       the ``PMIX_NSPACE`` attribute) in the specified session and recover
       all their resources. If no ``PMIX_NSPACE`` is specified, then preempt
       all jobs in the session.
       
   * - ``PMIX_SESSION_RESTORE``
     - ``pmix.ssn.restore``
     - ``(bool)``
     - restore indicated jobs (given in accompanying ``pmix_info_t`` via
       the ``PMIX_NSPACE`` attribute) in the specified session, including
       all their resources. If no ``PMIX_NSPACE`` is specified, then restore
       all jobs in the session.
       
   * - ``PMIX_SESSION_SIGNAL``
     - ``pmix.ssn.sig``
     - ``(int)``
     - send given signal to all processes of every job in the session

Session operational attributes |mdash| called by RTE.

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_SESSION_COMPLETE``
     - ``pmix.ssn.complete``
     - ``(bool)``
     - specified session has completed, all resources have been
       recovered and are available for scheduling. Must include
       ``pmix_info_t`` indicating ID and returned status of any jobs
       executing in the session.
       

Allocation directive values
^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_ALLOC_REQ_CANCEL`` (value: 5): Cancel the indicated allocation request

       
Datatypes
^^^^^^^^^

* ``pmix_value_cmp_t``: an enum indicating the relative value of
  two ``pmix_value_t objects``. Values include:
  
  * ``PMIX_EQUAL``
  * ``PMIX_VALUE1_GREATER``
  * ``PMIX_VALUE2_GREATER``
  * ``PMIX_VALUE_TYPE_DIFFERENT``
  * ``PMIX_VALUE_INCOMPATIBLE_OBJECTS``
  * ``PMIX_VALUE_COMPARISON_NOT_AVAIL``

* ``pmix_disk_stats_t``: contains statistics on disk read/write operations
* ``pmix_net_stats_t``: contains statistics on network activity
* ``pmix_node_stats_t``: contains statistics on node resource usage
* ``pmix_proc_stats_t``: contains statistics on process resource usage

  
Datatype static initializers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Static initializers were added for each complex data type (i.e., a data type
defined as a struct):

* ``PMIX_PROC_STATS_STATIC_INIT``
* ``PMIX_DISK_STATS_STATIC_INIT``
* ``PMIX_NET_STATS_STATIC_INIT``
* ``PMIX_NODE_STATS_STATIC_INIT``

  
Macros
^^^^^^

* ``PMIX_XFER_PROCID``: transfer a ``pmix_proc_t`` to another one
  (non-destructive copy)
* ``PMIX_INFO_SET_END``: mark this ``pmix_info_t`` as being at the end
  of an array
* ``PMIX_INFO_SET_PERSISTENT``: mark that the data in this
  ``pmix_info_t`` is not to be released by ``PMIX_Info_destruct()`` (or its
  macro form)
* ``PMIX_INFO_SET_QUALIFIER``: mark this ``pmix_info_t`` as a qualifier to the
  primary key
* ``PMIX_INFO_IS_PERSISTENT``: test if this ``pmix_info_t`` has been marked as persistent
* ``PMIX_INFO_IS_QUALIFIER``: test if this ``pmix_info_t`` has been marked as a qualifier
* ``PMIX_DATA_ARRAY_INIT``: initialize a ``pmix_data_array_t``
* ``PMIX_CHECK_TRUE``: check if a ``pmix_value_t`` is boolean ``true`` (supports
  string as well as traditional boolean values)
* ``PMIX_CHECK_BOOL``: check if a ``pmix_value_t`` is a boolean value (supports
  string as well as traditional boolean values)
  

Macros supporting ``pmix_disk_stats_t`` objects:

* ``PMIX_DISK_STATS_CONSTRUCT``
* ``PMIX_DISK_STATS_CREATE``
* ``PMIX_DISK_STATS_DESTRUCT``
* ``PMIX_DISK_STATS_FREE``
* ``PMIX_DISK_STATS_RELEASE``

Macros supporting ``pmix_net_stats_t`` objects:

* ``PMIX_NET_STATS_CONSTRUCT``
* ``PMIX_NET_STATS_CREATE``
* ``PMIX_NET_STATS_DESTRUCT``
* ``PMIX_NET_STATS_FREE``
* ``PMIX_NET_STATS_RELEASE``

Macros supporting ``pmix_node_stats_t`` objects:

* ``PMIX_NODE_STATS_CONSTRUCT``
* ``PMIX_NODE_STATS_CREATE``
* ``PMIX_NODE_STATS_DESTRUCT``
* ``PMIX_NODE_STATS_RELEASE``

Macros supporting ``pmix_proc_stats_t`` objects:

* ``PMIX_PROC_STATS_CONSTRUCT``
* ``PMIX_PROC_STATS_CREATE``
* ``PMIX_PROC_STATS_DESTRUCT``
* ``PMIX_PROC_STATS_FREE``
* ``PMIX_PROC_STATS_RELEASE``


Renamed Constants
-----------------

OpenPMIx version |opmix_ver| renamed the following constants:

* ``PMIX_DEBUG_WAIT_FOR_NOTIFY``: renamed to ``PMIX_READY_FOR_DEBUG``
