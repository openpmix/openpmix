.. _man3-PMIx_server_collect_job_info:

PMIx_server_collect_job_info
============================

.. include_body

``PMIx_server_collect_job_info`` |mdash| Collect the job-level information for
an array of processes into a data buffer.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_collect_job_info(pmix_proc_t *procs, size_t nprocs,
                                              pmix_data_buffer_t *dbuf);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``procs``: Array of :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures
  identifying the processes whose job-level information is to be collected. The
  library reduces the array to its set of unique namespaces and collects the
  job-level data for each.
* ``nprocs``: Number of elements in the ``procs`` array.


OUTPUT PARAMETERS
-----------------

* ``dbuf``: Pointer to a :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
  provided by the caller. On success, the buffer is loaded with the packed
  job-level information |mdash| for each participating namespace, the namespace
  name followed by its job-level key-value data, wrapped as a byte object. The
  caller owns the resulting buffer contents and must release them with
  ``PMIx_Data_buffer_destruct`` (or ``PMIx_Data_buffer_release``) when no longer
  needed.


DESCRIPTION
-----------

Collect the job-level information already held by the local PMIx server library
for the namespaces represented in the ``procs`` array, packing it into the
caller-provided data buffer for transmission or later replay. This is used, for
example, to bootstrap a peer server with the job-level data it needs for a set
of processes without requiring a separate registration for each namespace.

For each unique namespace in ``procs``, the library locates the namespace in its
internal tables and fetches its job-level data |mdash| preferring a local
client's data store when one exists, otherwise falling back to the server's own
storage. Namespaces the server does not know are silently skipped. The packed
result for each namespace is appended to ``dbuf`` as a byte object.

Unlike most server APIs, ``PMIx_server_collect_job_info`` is a **blocking**
operation and does not take a callback: internally it thread-shifts the request
into the library's progress thread, waits for completion, and returns the final
status directly. The output buffer is populated only on a ``PMIX_SUCCESS``
return.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the job-level information was collected and loaded
  into ``dbuf``.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the requested job-level information could not
  be located for the specified namespace(s).
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

On any non-success return, the contents of ``dbuf`` are unspecified and must
not be used. Any other negative value indicates an appropriate error condition.
PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

This is an OpenPMIx server-library extension; it is not part of the PMIx
Standard. It is available only after the server library has been initialized
with :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
