.. _man3-PMIx_Init:

PMIx_Init
=========

.. include_body

``PMIx_Init`` |mdash| Initialize the PMIx client library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Init(pmix_proc_t *proc,
                           pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_PROGRAMMING_MODEL,
             'value': {'value': "MPI", 'val_type': PMIX_STRING}}]
  rc, myname = foo.init(pydirs)


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the initialization (see
  `DIRECTIVES`_). A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


INPUT/OUTPUT PARAMETERS
-----------------------

* ``proc``: Pointer to a ``pmix_proc_t`` structure in which the client's
  namespace and rank are to be returned. Passing ``NULL`` is allowed if the
  caller only wishes to initialize the PMIx system and does not require the
  identifier at this time.


DESCRIPTION
-----------

Initialize the PMIx client, returning the process identifier assigned to this
client's application in the provided ``pmix_proc_t`` struct. Passing a value of
``NULL`` for the ``proc`` parameter is allowed if the caller wishes solely to
initialize the PMIx system and does not require return of the identifier at that
time.

When called, the PMIx client will check for the required connection information
of the local PMIx server and will establish the connection. If the information
is not found, or the server connection fails, then an appropriate error constant
will be returned.

If successful, the function will return ``PMIX_SUCCESS`` and will fill the
provided structure (if non-``NULL``) with the server-assigned namespace and rank
of the process within the application. In addition, all startup information
provided by the resource manager is made available to the client process via
subsequent calls to :ref:`PMIx_Get(3) <man3-PMIx_Get>`.

The ``info`` array is used to pass user directives pertaining to the
initialization and subsequent operations. These commonly describe the transport
by which the client is to rendezvous with the local server, or announce the
programming model and threading model being initialized so that the event
notification system can coordinate among cooperating libraries within the same
process (see `DIRECTIVES`_).

Reference counting
^^^^^^^^^^^^^^^^^^

The PMIx client library is reference counted, and so multiple calls to
``PMIx_Init`` are allowed. Thus, one way for an application process to obtain its
namespace and rank is to simply call ``PMIx_Init`` with a non-``NULL`` ``proc``
parameter. Each call to ``PMIx_Init`` must be balanced with a matching call to
:ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>` so that the connection to the local
server is only torn down once every initializing caller has finalized.

Multiple calls to ``PMIx_Init`` must not include conflicting directives; an error
is returned when directives that conflict with those of a prior call are
encountered.


DIRECTIVES
----------

The following attributes are relevant to this operation. Support for any given
attribute is optional and depends on how the PMIx implementation was built and
on the capabilities of the host environment.

Connection attributes
^^^^^^^^^^^^^^^^^^^^^^

These attributes describe how the client is to rendezvous with the local PMIx
server. They are consumed during connection establishment and are not typically
retrievable via :ref:`PMIx_Get(3) <man3-PMIx_Get>`.

* ``PMIX_TCP_URI`` (char\*) |mdash| the URI of the PMIx server to connect to, or
  a file name containing it in the form ``file:<name of file>``.
* ``PMIX_TCP_REPORT_URI`` (char\*) |mdash| direct that the client's TCP URI be
  reported, and indicate the method: ``'-'`` for stdout, ``'+'`` for stderr, or
  a filename.
* ``PMIX_TCP_IF_INCLUDE`` (char\*) |mdash| comma-delimited list of devices and/or
  CIDR notation to include when establishing the TCP connection.
* ``PMIX_TCP_IF_EXCLUDE`` (char\*) |mdash| comma-delimited list of devices and/or
  CIDR notation to exclude when establishing the TCP connection.
* ``PMIX_TCP_IPV4_PORT`` (int) |mdash| the IPv4 port to be used.
* ``PMIX_TCP_IPV6_PORT`` (int) |mdash| the IPv6 port to be used.
* ``PMIX_TCP_DISABLE_IPV4`` (bool) |mdash| set to ``true`` to disable the IPv4
  family of addresses.
* ``PMIX_TCP_DISABLE_IPV6`` (bool) |mdash| set to ``true`` to disable the IPv6
  family of addresses.
* ``PMIX_USOCK_DISABLE`` (bool) |mdash| if the library supports Unix socket
  connections, disable their use.
* ``PMIX_SOCKET_MODE`` (uint32_t) |mdash| set the mode of the rendezvous socket.
* ``PMIX_SINGLE_LISTENER`` (bool) |mdash| if the library supports more than one
  method for clients to connect to servers, use only one of them.

Runtime attributes
^^^^^^^^^^^^^^^^^^

* ``PMIX_GDS_MODULE`` (char\*) |mdash| select the generalized data store (GDS)
  component(s) the client is to use for storing and retrieving job-level and
  peer data. If not provided, the value of the ``PMIX_GDS_MODULE`` environment
  variable is honored, defaulting to the ``hash`` component.
* ``PMIX_EVENT_BASE`` (void\*) |mdash| pointer to an ``event_base`` to use in
  place of the internal progress thread. The event base **must** be compatible
  with the event library (e.g., libevent or libev) the PMIx implementation was
  built against.
* ``PMIX_EXTERNAL_PROGRESS`` (bool) |mdash| the host will progress the PMIx
  library as needed via calls to ``PMIx_Progress`` (see PMIx_Progress(3)),
  rather than PMIx spawning its own internal progress thread.

Programming-model attributes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When provided, the following attributes are used by the event notification
system for inter-library coordination |mdash| for example, so that an MPI and an
OpenMP runtime co-located in the same process can discover one another via a
``PMIX_MODEL_DECLARED`` event.

* ``PMIX_PROGRAMMING_MODEL`` (char\*) |mdash| programming model being initialized
  (e.g., ``"MPI"`` or ``"OpenMP"``).
* ``PMIX_MODEL_LIBRARY_NAME`` (char\*) |mdash| programming model implementation ID
  (e.g., ``"OpenMPI"`` or ``"MPICH"``).
* ``PMIX_MODEL_LIBRARY_VERSION`` (char\*) |mdash| programming model version string
  (e.g., ``"2.1.1"``).
* ``PMIX_THREADING_MODEL`` (char\*) |mdash| threading model used (e.g.,
  ``"pthreads"``).
* ``PMIX_MODEL_NUM_THREADS`` (uint64_t) |mdash| number of active threads being
  used by the model.
* ``PMIX_MODEL_NUM_CPUS`` (uint64_t) |mdash| number of CPUs being used by the
  model.
* ``PMIX_MODEL_CPU_TYPE`` (char\*) |mdash| granularity: ``"hwthread"``,
  ``"core"``, etc.
* ``PMIX_MODEL_AFFINITY_POLICY`` (char\*) |mdash| thread affinity policy, e.g.
  ``"master"``, ``"close"``, or ``"spread"``.


INITIALIZATION EVENTS
---------------------

The following events are typically associated with calls to ``PMIx_Init`` and may
be delivered to processes that have registered for them via
``PMIx_Register_event_handler`` (see PMIx_Register_event_handler(3)):

* ``PMIX_MODEL_DECLARED`` |mdash| a programming model has been declared by a call
  to ``PMIx_Init`` that carried the programming-model attributes above.
* ``PMIX_MODEL_RESOURCES`` |mdash| resource usage by a programming model has
  changed.
* ``PMIX_OPENMP_PARALLEL_ENTERED`` |mdash| an OpenMP parallel code region has been
  entered.
* ``PMIX_OPENMP_PARALLEL_EXITED`` |mdash| an OpenMP parallel code region has
  completed.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library could not be initialized (for
  example, a required transport was explicitly disabled, or a tight race between
  concurrent ``PMIx_Init`` calls left the library in an unusable state).
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| a provided directive requested behavior the
  implementation does not support.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_Init`` is intended for use by *client* processes |mdash| processes that
were registered with a PMIx server before being started. Processes acting as
*tools* (debuggers, profilers, workflow managers) should instead call
``PMIx_tool_init``, and processes hosting a PMIx server should call
``PMIx_server_init``.


.. seealso::
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   :ref:`PMIx_Initialized(3) <man3-PMIx_Initialized>`,
   PMIx_Progress(3),
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   PMIx_tool_init(3),
   PMIx_server_init(3),
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
