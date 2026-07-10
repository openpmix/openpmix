.. _man3-PMIx_server_define_process_set:

PMIx_server_define_process_set
==============================

.. include_body

``PMIx_server_define_process_set`` |mdash| Define a PMIx process set.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_define_process_set(const pmix_proc_t *members,
                                                size_t nmembers,
                                                const char *pset_name);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # members is a list of proc dicts, each {'nspace': ns, 'rank': r}
  members = [{'nspace': 'myjob', 'rank': 0},
             {'nspace': 'myjob', 'rank': 1}]
  rc = foo.define_process_set(members, 'myset')


INPUT PARAMETERS
----------------

* ``members``: Pointer to an array of :ref:`pmix_proc_t(5)
  <man5-pmix_proc_t>` structures containing the identifiers of the
  processes that are members of the process set.
* ``nmembers``: Number of elements in the ``members`` array.
* ``pset_name``: NULL-terminated string name of the process set being
  defined.


DESCRIPTION
-----------

Provide a function by which the host environment can create a named
*process set* |mdash| a user-defined grouping of processes identified by a
string name and an associated membership list. Unlike a PMIx group (see
``PMIx_Group_construct``), a process set is purely a label applied by the
host environment; it establishes no collective context and requires no
participation by the member processes.

When called, the PMIx server library records the process set (name and
membership) so that it can later respond to client queries such as
``PMIX_QUERY_NUM_PSETS`` and ``PMIX_QUERY_PSET_NAMES``, and it alerts all
local clients to the new process set by generating a
``PMIX_PROCESS_SET_DEFINE`` event. The event carries the ``PMIX_PSET_NAME``
attribute (the set name) and the ``PMIX_PSET_MEMBERS`` attribute (a
``pmix_data_array_t`` of the member ``pmix_proc_t`` identifiers).

``PMIx_server_define_process_set`` is a blocking call. Internally it
thread-shifts the request onto the PMIx progress thread, records the set,
emits the local notification, and returns once that processing is
complete. The input ``members`` array need only remain valid for the
duration of the call; the library retains its own copy of the membership.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the process set was defined and the local
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
process set membership across all involved PMIx servers, and for ensuring
that process set names do not conflict with system-assigned namespaces
within the scope of the set. The PMIx library only records and advertises
the definition on the local server; it does not propagate it to peer
servers.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_delete_process_set(3) <man3-PMIx_server_delete_process_set>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
