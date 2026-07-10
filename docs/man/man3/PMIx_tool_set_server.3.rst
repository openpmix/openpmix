.. _man3-PMIx_tool_set_server:

PMIx_tool_set_server
====================

.. include_body

``PMIx_tool_set_server`` |mdash| Designate a server as the tool's
*primary* server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_set_server(const pmix_proc_t *server,
                                      pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # ... after a successful foo.init() and attachment to one or more servers ...
  server = {'nspace': "server-nspace", 'rank': 0}
  pydirs = [{'key': PMIX_WAIT_FOR_CONNECTION,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc = foo.set_server(server, pydirs)


INPUT PARAMETERS
----------------

* ``server``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
  structure containing the namespace and rank of the target server. The
  server **must** be one to which the tool is already connected (for
  example, via :ref:`PMIx_tool_attach_to_server(3)
  <man3-PMIx_tool_attach_to_server>`).
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see
  `DIRECTIVES`_). A ``NULL`` value is supported when no directives are
  desired.
* ``ninfo``: Number of elements in the ``info`` array.


DESCRIPTION
-----------

Designate the specified server to be the tool's *primary* server for all
subsequent PMIx API calls. A tool may be connected to several PMIx servers
simultaneously; the primary server is the one to which non-directed
requests (queries, spawns, event notifications, and the like) are routed by
default. ``PMIx_tool_set_server`` changes that default to the indicated
server.

This is a *blocking* call. Internally, the request is thread-shifted onto
the PMIx progress thread so that it may safely access the library's global
connection state, and the function does not return until the operation has
completed. The return value carries the result.

If the tool is not yet connected to the named server, the behavior depends
on the directives supplied. When ``PMIX_WAIT_FOR_CONNECTION`` is given, the
library will wait |mdash| up to any specified ``PMIX_TIMEOUT`` |mdash| for
the connection to be established before designating it as primary.


DIRECTIVES
----------

The following attributes are required to be supported by all PMIx libraries.

* ``PMIX_WAIT_FOR_CONNECTION`` (bool) |mdash| wait until the specified
  connection has been made before completing the operation.
* ``PMIX_TIMEOUT`` (int) |mdash| time, in seconds, before the operation
  should time out and return an error. A value of zero indicates that the
  operation should never time out.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the designated server has successfully been
made the tool's primary server. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx tool library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced
  because the library's progress engine has been stopped.
* ``PMIX_ERR_UNREACH`` |mdash| the specified server could not be reached.
* ``PMIX_ERR_TIMEOUT`` |mdash| the operation did not complete within the
  time specified by ``PMIX_TIMEOUT``.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

PMIx does not currently support on-the-fly changes to a tool's own
identifier. Accordingly, any server designated as primary must be under the
same namespace manager (e.g., the same host resource manager) as the tool's
original server, so that the tool's assigned namespace and rank remain a
unique, valid assignment.

This function is available only to processes that initialized as *tools*
via :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
