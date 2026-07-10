.. _man3-PMIx_server_setup_fork:

PMIx_server_setup_fork
======================

.. include_body

``PMIx_server_setup_fork`` |mdash| Set up the environment of a child
process that is about to be forked by the host environment.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_setup_fork(const pmix_proc_t *proc, char ***env);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  proc = {'nspace': "myapp", 'rank': 0}
  # envin is a dict that will be updated in place with the
  # PMIx environment variables the child requires
  envin = {}
  rc = foo.setup_fork(proc, envin)


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifying
  the namespace and rank of the client process that is about to be launched.


INPUT/OUTPUT PARAMETERS
-----------------------

* ``env``: Address of the environment array (a ``NULL``-terminated ``argv``-style
  array of ``"name=value"`` strings) for the child process. The array is updated
  in place: the required PMIx environment variables are added (existing values of
  the same name are overwritten). The updated array is what the host must pass to
  the child at ``fork``/``exec``.


DESCRIPTION
-----------

Set up the environment of a child process that the host environment is about to
fork so that the child's PMIx client library can correctly connect back to, and
interact with, this PMIx server. The host environment is **required** to call
this function for each local client prior to starting that client process.

A PMIx client needs a modest amount of setup information in its environment in
order to find and rendezvous with its local server. ``PMIx_server_setup_fork``
populates ``env`` with the necessary variables, including (among others) the
client's namespace and rank, the server's rendezvous connection URI, the active
security mode, the buffer type and generalized-datastore module in use, the
server's hostname, and the library version. It additionally appends any
contributions from the transport, network (``pnet``), datastore (``gds``), and
programming-model (``pmdl``) subsystems |mdash| for example, temporary-directory
settings or the variables a fabric library requires for "instant on" addressing
|mdash| along with any global environment variables the host registered for
distribution to all clients.

This is a **blocking** operation performed entirely within the local library; it
does not involve the host environment or a callback. The variables added here are
the counterpart to those the client library consumes during
:ref:`PMIx_Init(3) <man3-PMIx_Init>`.

Calling ``PMIx_server_setup_fork`` for the first local client of a namespace is
also the signal that causes any operations cached by a prior call to
:ref:`PMIx_server_setup_local_support(3) <man3-PMIx_server_setup_local_support>`
to be executed.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, with ``env`` updated in place. Otherwise a
negative PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized (see
  :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`).
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.

Any other negative value indicates that one of the contributing subsystems
(transport, network, datastore, or programming model) failed to add its
environmental contribution. PMIx error constants are defined in
``pmix_common.h``.


NOTES
-----

This function is a server-role API and is available only after
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. The host environment owns the
``env`` array and is responsible for freeing it.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   :ref:`PMIx_server_setup_local_support(3) <man3-PMIx_server_setup_local_support>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
