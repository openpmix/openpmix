.. _man3-PMIx_Get_cpuset:

PMIx_Get_cpuset
===============

.. include_body

``PMIx_Get_cpuset`` |mdash| Get the processing-unit (PU) binding bitmap of the
current process.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Get_cpuset(pmix_cpuset_t *cpuset,
                                 pmix_bind_envelope_t ref);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  rc, cpuset = foo.get_cpuset(PMIX_CPUBIND_PROCESS)


INPUT PARAMETERS
----------------

* ``ref``: The binding envelope of type ``pmix_bind_envelope_t`` to be
  considered when formulating the bitmap. Valid values are:

  * ``PMIX_CPUBIND_PROCESS`` (0) |mdash| use the location of all threads in the
    (possibly multi-threaded) process.
  * ``PMIX_CPUBIND_THREAD`` (1) |mdash| use only the location of the thread
    calling the API.


OUTPUT PARAMETERS
-----------------

* ``cpuset``: Pointer to a ``pmix_cpuset_t`` object where the bitmap is to be
  stored. On success, the object's ``source`` and ``bitmap`` fields are
  populated; the caller is responsible for releasing the associated storage
  with ``PMIx_Cpuset_destruct`` when done. If the caller pre-sets the
  ``source`` field, it must match the underlying binding source (e.g.,
  ``"hwloc"``).


DESCRIPTION
-----------

Obtain the PU binding location of the calling process from the local topology
and store it in the provided ``pmix_cpuset_t`` object based on the specified
binding envelope. ``PMIX_CPUBIND_PROCESS`` considers all threads of the
process, while ``PMIX_CPUBIND_THREAD`` considers only the calling thread.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, with the bitmap loaded into ``cpuset``.
On error, a negative value corresponding to a PMIx error constant is
returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an unrecognized ``ref`` binding envelope was
  supplied.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the binding information could not be obtained
  from the underlying topology.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

A process whose threads are not all bound to the same location may return
inconsistent results from calls made by different threads when the
``PMIX_CPUBIND_THREAD`` envelope is used.

Unlike most PMIx APIs, ``PMIx_Get_cpuset`` does not accept an array of
:ref:`pmix_info_t(5) <man5-pmix_info_t>` directives. This is an intentional
exception: the operation queries the local topology directly and takes no
attributes.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Parse_cpuset_string(3) <man3-PMIx_Parse_cpuset_string>`,
   :ref:`PMIx_Get_relative_locality(3) <man3-PMIx_Get_relative_locality>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_bind_envelope_t(5) <man5-pmix_bind_envelope_t>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
