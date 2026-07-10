.. _man3-PMIx_server_delete_process_set:

PMIx_server_delete_process_set
==============================

.. include_body

``PMIx_server_delete_process_set`` |mdash| Delete a PMIx process set name.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_delete_process_set(char *pset_name);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  rc = foo.delete_process_set('myset')


INPUT PARAMETERS
----------------

* ``pset_name``: NULL-terminated string name of the process set being
  deleted.


DESCRIPTION
-----------

Provide a function by which the host environment can delete a process set
name previously created with
:ref:`PMIx_server_define_process_set(3)
<man3-PMIx_server_define_process_set>`. Deleting the name has no impact on
the member processes themselves |mdash| it simply removes the label and
its associated membership record from the local PMIx server.

When called, the PMIx server library alerts all local clients to the
deletion by generating a ``PMIX_PROCESS_SET_DELETE`` event carrying the
``PMIX_PSET_NAME`` attribute (the name of the deleted set), then removes
the corresponding entry from its internal list of process sets. If no set
with the given name is currently recorded, the notification is still
issued and the call completes successfully.

``PMIx_server_delete_process_set`` is a blocking call. Internally it
thread-shifts the request onto the PMIx progress thread, emits the local
notification, removes the recorded set, and returns once that processing
is complete.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the deletion was processed and the local
  notification was issued.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been
  initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the library's progress engine has
  been stopped, so the request cannot be serviced.

PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

This API is restricted to the server role and must be called only after a
successful :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`.

The host environment is responsible for ensuring consistent knowledge of
process set membership across all involved PMIx servers. The PMIx library
only removes the definition on the local server; it does not propagate the
deletion to peer servers.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_define_process_set(3) <man3-PMIx_server_define_process_set>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
