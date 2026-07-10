.. _man3-PMIx_server_register_nspace:

PMIx_server_register_nspace
===========================

.. include_body

``PMIx_server_register_nspace`` |mdash| Register a namespace and its job-level
information with the PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_register_nspace(const pmix_nspace_t nspace, int nlocalprocs,
                                             pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init(pydirs, map) ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_UNIV_SIZE,
             'value': {'value': 4, 'val_type': PMIX_UINT32}},
            {'key': PMIX_JOB_SIZE,
             'value': {'value': 4, 'val_type': PMIX_UINT32}}]
  rc = foo.register_nspace("myjob", 4, pydirs)


INPUT PARAMETERS
----------------

* ``nspace``: The namespace (a character array of maximum length
  ``PMIX_MAX_NSLEN``) of the job being registered.
* ``nlocalprocs``: The number of processes from this namespace that will be
  launched locally |mdash| i.e., that will connect to this PMIx server. This
  count is required so that the server library can correctly determine when a
  collective operation is locally complete, even if the collective is called
  before all local processes have started.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying the session-, job-, application-, node-, and
  process-realm information for the namespace (see `DIRECTIVES`_). A ``NULL``
  value is supported when no data is to be registered (for example, when using
  the ``PMIX_REGISTER_NODATA`` directive).
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked when the
  registration completes. A ``NULL`` value makes the call *blocking* (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Register a namespace (job) with the PMIx server library so that its job-level
information can be provided to the processes in that job as they connect. The
PMIx connection procedure gives the host PMIx server an opportunity to pass
job-related information down to each child process |mdash| for example, the
number of processes in the job, the relative local ranks of the processes, and
the node and process maps. The host is free to determine which of the supported
elements it provides; the defined values are described in `DIRECTIVES`_.

The host **must** register *every* namespace that will participate in
collective operations involving local processes |mdash| even a namespace from
which this server hosts no local processes |mdash| if any local process might
at some point perform a collective operation involving one or more processes
from that namespace. This is required so the collective can determine when it
is locally complete.

Blocking and non-blocking forms
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``PMIx_server_register_nspace`` supports both a non-blocking and a blocking
mode of operation, selected by the ``cbfunc`` argument. This function is the
canonical example of the PMIx thread-shifting pattern: in both modes the
request parameters are packaged and posted to the library's internal progress
thread, where the actual registration work is performed.

When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*: the function
posts the request to the progress thread and returns ``PMIX_SUCCESS``
immediately, and the provided ``cbfunc`` is invoked with the final status once
registration completes. As with all non-blocking PMIx APIs, the caller **must**
keep the ``info`` array valid until ``cbfunc`` has been invoked.

When ``cbfunc`` is ``NULL``, the call is *blocking*: the library substitutes an
internal callback, posts the request, and waits for the progress thread to
finish before returning. In this case the result is carried by the return value
itself, and on success the function returns ``PMIX_OPERATION_SUCCEEDED`` to
indicate that the operation completed inline and no callback will fire.


DIRECTIVES
----------

Information is passed through the ``info`` array, organized by *data realm*.
Realm information may be passed either as individual
:ref:`pmix_info_t(5) <man5-pmix_info_t>` entries or grouped inside a
``pmix_data_array_t`` labeled with the corresponding ``*_INFO_ARRAY``
attribute. The following attributes are required to be supported by all PMIx
libraries for use with this API:

* ``PMIX_REGISTER_NODATA`` (bool) |mdash| the registration is for the namespace
  only; do not copy any job data. Used to pre-register a namespace whose data
  will follow later.
* ``PMIX_SESSION_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array of
  session-realm information for the session containing this job.
* ``PMIX_JOB_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array of job-realm
  information for this namespace.
* ``PMIX_APP_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array of
  application-realm information; required (one array per application) when the
  job contains more than one application.
* ``PMIX_PROC_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array of
  process-realm information for a process in the job.
* ``PMIX_NODE_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array of node-realm
  information for a node participating in the job.

Session-realm information
^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_UNIV_SIZE`` (uint32_t) |mdash| number of process slots allocated to
  the session.
* ``PMIX_MAX_PROCS`` (uint32_t) |mdash| maximum number of processes; must be
  provided if ``PMIX_UNIV_SIZE`` is not given.
* ``PMIX_SESSION_ID`` (uint32_t) |mdash| session identifier; required whenever
  the PMIx server library may host multiple sessions.

Job-realm information
^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_NSPACE`` (char\*) |mdash| namespace of the job being registered.
* ``PMIX_JOBID`` (char\*) |mdash| job identifier assigned by the resource
  manager.
* ``PMIX_JOB_SIZE`` (uint32_t) |mdash| total number of processes in the job.
* ``PMIX_NODE_MAP`` (char\*) |mdash| regular-expression representation of the
  nodes hosting the job (see ``PMIx_generate_regex2``).
* ``PMIX_PROC_MAP`` (char\*) |mdash| regular-expression representation of the
  process-to-node mapping.
* ``PMIX_JOB_NUM_APPS`` (uint32_t) |mdash| number of applications in the job;
  required when the job contains more than one application.
* ``PMIX_SERVER_NSPACE`` (char\*) |mdash| namespace of the PMIx server itself.
* ``PMIX_SERVER_RANK`` (pmix_rank_t) |mdash| rank of the PMIx server itself.

The host environment is expected to supply a broad range of additional
session-, job-, application-, node-, and process-realm information as required
by the job. See the "Reserved Keys" chapter of the PMIx Standard for the full
list and for the rules governing how each key is retrieved.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` from its progress thread once registration
completes. ``status`` is ``PMIX_SUCCESS`` if the namespace and its data were
registered successfully, or a negative PMIx error constant otherwise.
``cbdata`` is the opaque pointer passed to ``PMIx_server_register_nspace``,
allowing the host to correlate the completion with its originating request.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` is non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status is delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), the return value carries the
result directly:

* ``PMIX_OPERATION_SUCCEEDED`` |mdash| the registration completed successfully
  inline; no callback is or will be invoked.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

There is no requirement that a namespace be registered before its clients are
registered; if
:ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>` is
called for an as-yet-unregistered namespace, the library creates a placeholder
namespace in anticipation of the eventual ``PMIx_server_register_nspace`` call.
However, collective operations cannot complete locally until the namespace has
been registered with its ``nlocalprocs`` count.

Large ``PMIX_NODE_MAP`` and ``PMIX_PROC_MAP`` values are commonly generated in
compressed form using ``PMIx_generate_regex2`` prior to registration.


.. seealso::
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`,
   :ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`,
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
