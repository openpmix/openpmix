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
             'value': "MPI", 'val_type': PMIX_STRING}]
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

The identity variables
^^^^^^^^^^^^^^^^^^^^^^

The host tells a client who it is and how to reach its server through
environment variables. ``PMIx_Init`` takes them out of the process's
environment as it begins, before it reads them, and keeps them for this
process:

* ``PMIX_NAMESPACE`` and ``PMIX_RANK``, and ``PMIX_SERVER_NSPACE`` and
  ``PMIX_SERVER_RANK``;
* every ``PMIX_SERVER_URI`` variable (``PMIX_SERVER_URI41``,
  ``PMIX_SERVER_URI5``, and so on) and ``PMIX_SERVER_LOCAL_URI``;
* ``PMIX_CONNECT_TOKEN``, whose value is also overwritten where the
  environment held it;
* ``PMIX_LAUNCHER_RNDZ_URI``, ``PMIX_SECURITY_MODE``,
  ``PMIX_BFROP_BUFFER_TYPE``, ``PMIX_GDS_MODULE`` and ``PMIX_VERSION``.

A process the application starts after that - forked, or forked and
exec'd - and a program it execs inherit none of them. Such a process that
calls ``PMIx_Init`` is a singleton; one that calls
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` is a tool the server's host
names. A later ``PMIx_Init`` of the same process, after
:ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`, uses the values kept. The
other variables stay: the server's temporary directories
(``PMIX_SERVER_TMPDIR``, ``PMIX_SYSTEM_TMPDIR``), ``PMIX_HOSTNAME``, MCA
parameters, and those that programming-model support sets.

Code that reads these variables itself must do so before the first
``PMIx_Init``. After it, the process's namespace and rank are the ones
``PMIx_Init`` returns.

Only the C library's environment is changed. A language runtime that keeps
a copy of its own - Python's ``os.environ``, made when the interpreter
starts, is one - still holds the variables, and passes them on to a
process it starts from that copy. The PMIx Python bindings take them out
of ``os.environ`` as well. Other code that starts processes from such a
copy must leave them out itself.

.. warning:: Use PMIx with caution in a setuid or setgid program. PMIx
   takes its configuration from the environment and from locations the
   environment names: the ``PMIX_*`` variables (including those that name
   the server to connect to and the directories components are loaded
   from), a per-user parameter file under ``$HOME``, and the session
   directory under ``$TMPDIR``. In a setuid or setgid program these are
   set by the user who ran it, not by the program's owner. PMIx cannot
   tell the difference, and it cannot ignore them, because the
   environment is how a process finds its server. A setuid or setgid
   program may need to set or clear these variables itself before calling
   this function.


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

A client given ``PMIX_SERVER_LOCAL_URI`` by its server connects to the
server's local socket.
``PMIX_SOCKET_MODE`` sets the mode of a server's local socket and has no
effect here (see :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`); the
deprecated ``PMIX_USOCK_DISABLE`` and ``PMIX_SINGLE_LISTENER`` have no effect.

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
* ``PMIX_BIND_PROGRESS_THREAD`` (char\*) |mdash| comma-delimited ranges of CPUs to
  which the internal PMIx progress thread is to be bound.
* ``PMIX_BIND_REQUIRED`` (bool) |mdash| return an error if the internal PMIx
  progress thread cannot be bound as requested.
* ``PMIX_EXTERNAL_AUX_EVENT_BASE`` (void\*) |mdash| pointer to an ``event_base``
  the library is to use for auxiliary functions (e.g., capturing signals) that
  would otherwise interfere with the host.

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
* ``PMIX_MODEL_PHASE_NAME`` (char\*) |mdash| user-assigned name for a phase in the
  application's execution (e.g., ``"cfd reduction"``).
* ``PMIX_MODEL_PHASE_TYPE`` (char\*) |mdash| type of phase being executed (e.g.,
  ``"matrix multiply"``).


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
* ``PMIX_ERR_UNREACH`` |mdash| no PMIx server was described to this process
  (neither by its environment nor by a ``PMIX_SERVER_URI`` directive) and no
  system server was found. Note that this is **not** a failure: the library is
  fully initialized and running as a singleton, the caller's identity has been
  assigned, and :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>` must still be
  called. Operations that require a server return ``PMIX_ERR_UNREACH`` in their
  turn, while those the library can satisfy on its own behave normally.
* ``PMIX_ERR_COMM_FAILURE`` |mdash| a PMIx server *was* described to this
  process |mdash| typically in the environment set up by the launcher that
  started it, or by a ``PMIX_SERVER_URI`` directive |mdash| but a connection to
  it could not be established. This process was set up to run under that
  server, so it does not fall back to running as a singleton. Unlike
  ``PMIX_ERR_UNREACH`` above, the library is **not** initialized.
* ``PMIX_ERR_OUTDATED`` |mdash| a PMIx server was found, but it uses a version
  of the PMIx library older than this one can connect to (v3.2). A message
  naming the server's version is printed. The library is **not** initialized.
* ``PMIX_ERR_LOST_CONNECTION`` |mdash| a server was reached, but the connection
  failed before it delivered the caller's job information. Unlike
  ``PMIX_ERR_UNREACH`` above, the library is **not** initialized.
* ``PMIX_ERR_EXISTS`` |mdash| the server already has a connection for this
  process's namespace and rank, from another process. A server accepts one
  connection per rank, and the first process to connect holds it: a process
  that a client started before its ``PMIx_Init``, and that inherited the
  client's environment, cannot initialize as the client's rank, and an application started by a job script
  cannot initialize as the script's rank if a tool the script started took it
  first (see :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`). A process that
  execs closes its connection as it does, so the program it execs is not
  refused for that.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the environment describing this process is
  malformed. In particular, ``PMIX_NAMESPACE`` was present but the accompanying
  ``PMIX_RANK`` was empty, negative, non-numeric, or not a valid rank value.
* ``PMIX_ERR_WOULD_BLOCK`` |mdash| returned to a *subsequent* ``PMIx_Init`` made
  from within a PMIx event handler or callback. Such a call may have to wait on
  the PMIx progress thread, which is the thread it would be running on.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| a provided directive requested behavior the
  implementation does not support.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.

A ``PMIx_Init`` that returns an error other than ``PMIX_ERR_UNREACH`` leaves the
library uninitialized and gives back everything it had allocated, so the caller
may correct whatever was wrong |mdash| a directive, an environment variable, the
server it was pointed at |mdash| and call ``PMIx_Init`` again. It must **not**
call :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>` to clean up after a call that
did not succeed; there is nothing left to finalize.


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
   :ref:`PMIx_Progress(3) <man3-PMIx_Progress>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   PMIx_tool_init(3),
   PMIx_server_init(3),
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
