.. _man3-PMIx_Resolve_nodes:

PMIx_Resolve_nodes
==================

.. include_body

``PMIx_Resolve_nodes`` |mdash| Return the list of nodes hosting processes within a
given namespace.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Resolve_nodes(const pmix_nspace_t nspace, char **nodelist);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # nspace is a string; it may be None to request all namespaces
  rc, nodes = foo.resolve_nodes("testnspace")
  # nodes is a comma-delimited string of node names


INPUT PARAMETERS
----------------

* ``nspace``: The namespace whose hosting nodes are to be resolved. A ``NULL``
  (zero-length) namespace requests the aggregate list of nodes across **all**
  namespaces known to the library.


OUTPUT PARAMETERS
-----------------

* ``nodelist``: Address of a ``char`` pointer. On success it is set to a freshly
  allocated, comma-delimited string of the names of the nodes hosting processes in
  the specified namespace. If no such nodes are found, it is set to ``NULL``. The
  caller is responsible for releasing the string with ``free`` (or, from Python,
  the binding releases it automatically).


DESCRIPTION
-----------

Given an ``nspace``, return the list of nodes hosting processes within that
namespace. The returned string contains a comma-delimited list of node names. If
``nspace`` is ``NULL``, the returned list is the union of the nodes hosting
processes across every namespace known to the library, with duplicate node names
removed. If no matching nodes are found, ``nodelist`` is set to ``NULL`` and the
call still returns ``PMIX_SUCCESS``.

``PMIx_Resolve_nodes`` is a blocking operation and is provided only in blocking
form. It is a convenience routine that gives simplified access to a commonly
requested query; because of that simplified interface it accepts no attributes and
cannot be further customized. When more specialized control is required, the same
information can typically be obtained through
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>` using the
``PMIX_QUERY_RESOLVE_NODE`` key.

As with :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>`, the library answers
the request from the most authoritative source available. When the caller is a
PMIx server, the request is first passed to the host environment; when the caller
is a client connected to its local server, the request is forwarded to that server.
If the authoritative source cannot satisfy the request |mdash| for example, an
older peer that does not recognize the operation |mdash| the library falls back to
computing the answer from its own local knowledge. A caller that is not connected
to a server is answered entirely from local data.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, in which case ``nodelist`` has been set as
described above (possibly to ``NULL`` if no matching nodes were found). On error, a
negative value corresponding to a PMIx error constant is returned, including:

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

The returned ``nodelist`` string is owned by the caller and must be released with
``free`` when no longer needed. A ``NULL`` result accompanied by ``PMIX_SUCCESS``
is the normal indication that no nodes host processes in the requested namespace
|mdash| it is not an error.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
