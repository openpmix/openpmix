.. _man3-PMIx_Resolve_peers:

PMIx_Resolve_peers
==================

.. include_body

``PMIx_Resolve_peers`` |mdash| Return the array of processes within a specified
namespace that are executing on a given node.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Resolve_peers(const char *nodename,
                                    const pmix_nspace_t nspace,
                                    pmix_proc_t **procs, size_t *nprocs);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # nodename and nspace are strings; either may be None
  rc, peers = foo.resolve_peers("node01", "testnspace")
  # peers is a list of Python ``pmix_proc_t`` dictionaries


INPUT PARAMETERS
----------------

* ``nodename``: NULL-terminated name of the node to query. A ``NULL`` value
  denotes the caller's own local node.
* ``nspace``: The namespace whose processes are to be resolved on the node. A
  ``NULL`` (zero-length) namespace requests **all** processes on the node,
  regardless of namespace.


OUTPUT PARAMETERS
-----------------

* ``procs``: Address of a ``pmix_proc_t`` pointer. On success it is set to a
  freshly allocated array of process identifiers executing on the specified node.
  If the node currently hosts no matching processes, the array is set to ``NULL``.
  The caller is responsible for releasing the array with ``PMIX_PROC_FREE``.
* ``nprocs``: Address where the number of elements in the ``procs`` array is
  returned. It is set to zero when no matching processes are found.


DESCRIPTION
-----------

Given a ``nodename``, return the array of processes within the specified
``nspace`` that are executing on that node. If ``nspace`` is ``NULL``, then all
processes on the node are returned, aggregated across every namespace known to the
library. If the specified node does not currently host any matching processes,
then ``procs`` is set to ``NULL`` and ``nprocs`` to zero |mdash| this is reported
as ``PMIX_SUCCESS``, not an error.

``PMIx_Resolve_peers`` is a blocking operation and is provided only in blocking
form. It is a convenience routine that gives simplified access to a commonly
requested query; because of that simplified interface it accepts no attributes and
cannot be further customized. When more specialized control is required, the same
information can typically be obtained through
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>` using the
``PMIX_QUERY_RESOLVE_PEERS`` key.

The library answers the request from the most authoritative source available. When
the caller is a PMIx server, the request is first passed to the host environment
(which has a more global view); when the caller is a client that is connected to
its local server, the request is forwarded to that server. In either case, if the
authoritative source cannot satisfy the request |mdash| for example, an older peer
that does not recognize the operation |mdash| the library falls back to computing
the answer from its own local knowledge. A caller that is not connected to a server
is answered entirely from local data.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, in which case ``procs`` and ``nprocs`` have
been set as described above (possibly to ``NULL`` and zero if the node hosts no
matching processes). On error, a negative value corresponding to a PMIx error
constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_UNREACH`` |mdash| the connection to the local PMIx server was lost
  while servicing the request.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid response was received while servicing
  the request.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The returned ``procs`` array is owned by the caller and must be released with
``PMIX_PROC_FREE`` when no longer needed. An empty result (``NULL`` array,
``nprocs`` of zero) accompanied by ``PMIX_SUCCESS`` is the normal indication that
the node hosts no processes in the requested namespace |mdash| it is not an error.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Resolve_nodes(3) <man3-PMIx_Resolve_nodes>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
