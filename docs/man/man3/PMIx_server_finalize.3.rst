.. _man3-PMIx_server_finalize:

PMIx_server_finalize
====================

.. include_body

``PMIx_server_finalize`` |mdash| Finalize the PMIx server support library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_finalize(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init(pydirs, map) ...
  rc = foo.finalize()


DESCRIPTION
-----------

Finalize the PMIx server support library, terminating all connections to
attached tools and any local clients. If internal communications are in use,
the server shuts them down at this time, and all memory usage by the library is
released.

The PMIx server library is reference counted. Because multiple calls to
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>` are permitted, each must be
balanced by a matching call to ``PMIx_server_finalize``. Only the final call
|mdash| the one that drops the reference count to zero |mdash| actually tears
down the library; earlier calls simply decrement the count and return
``PMIX_SUCCESS`` without releasing resources.

As part of teardown, the final call stops the internal progress thread, flushes
any residual forwarded I/O to its channels, stops listening for new
connections, releases all per-client and per-namespace state (executing any
registered cleanup epilogs), and closes the frameworks opened during
initialization.

``PMIx_server_finalize`` is a blocking call that completes synchronously; it
does not take a callback.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding
to a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library was not initialized, so
  there is nothing to finalize.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_server_finalize`` is intended for use only by processes that previously
called :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. It should be the last
PMIx server call made by the host; no server-side PMIx operations should be
issued after the reference count reaches zero and the library has been torn
down.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
