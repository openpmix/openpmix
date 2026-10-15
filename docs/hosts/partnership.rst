.. _label-host-env-partnership:

What a Host Must Do
===================

This page lists what every host environment must do to support PMIx,
in the order a host meets each item. The pages that follow cover
security (:doc:`connections`, :doc:`authorization`) and the division
of work for each operation (:doc:`operations`).


Building and running
--------------------

* **Build the host against the PMIx library it will run against.**
  This is the one exception to PMIx's forward compatibility.
  ``PMIx_server_init`` receives only a *pointer* to your
  ``pmix_server_module_t``. The module keeps every function pointer it
  has ever had, but it grows as up-calls are added, and the library
  cannot tell from a pointer how large your module is or which version
  it came from. It assumes your module matches its own headers, and
  that your up-calls behave as its version specifies. So when PMIx is
  upgraded, rebuild the host. New members are added to the module only
  in a new major release, so a host that runs against a library of a
  different major version is not supported.
* **Applications, libraries, and tools need no rebuild.** PMIx is
  forward compatible: every public API, macro, structure, and attribute
  is kept forever. A program built against an earlier PMIx builds and
  runs against a later one without change.
* **Clients and tools need not match the server's version** at run
  time either: a server accepts clients and tools from v2.1 on. The
  library handles that compatibility; the host does nothing for it.
  See :ref:`label-version-numbers`.
* **Check for features with capability flags,** not version numbers.
  Each ``PMIX_CAP_*`` flag in ``pmix_version.h`` says a feature is
  present. For example, ``PMIX_CAP_REQUESTER_ID`` says up-calls carry
  the requester's identity, and ``PMIX_CAP_DMODEX_REQUEST2`` says
  :ref:`PMIx_server_dmodex_request2(3) <man3-PMIx_server_dmodex_request2>`
  exists.


Starting the server
-------------------

Call :ref:`PMIx_server_init(3) <man3-PMIx_server_init>` with your
module and the directives that describe your server. The directives
decide, among other things:

* whether tools may connect, and how they find the server
  (``PMIX_SERVER_TOOL_SUPPORT``, ``PMIX_SERVER_SYSTEM_SUPPORT``,
  ``PMIX_SERVER_SESSION_SUPPORT``);
* whether tools running as other users may connect
  (``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``);
* whether the server listens on interfaces other than loopback
  (``PMIX_SERVER_REMOTE_CONNECTIONS``, in a PMIx configured
  ``--enable-remote-tools``);
* where the server's rendezvous files and session directories go
  (``PMIX_SERVER_TMPDIR``, ``PMIX_SYSTEM_TMPDIR``).

These are the host's decisions. A system administrator cannot change
them through MCA parameters (see
:ref:`label-sysadmin-authentication`). Whether the server takes clients
and tools of earlier releases is the host's when it says, with
``PMIX_SERVER_LEGACY_PEERS``, and the ``ptl_base_legacy_peers``
parameter's when it does not. A host that wants tools
of other users to connect must also give the server a directory those
users can search; see :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`
and :doc:`/how-things-work/session_dirs`.

.. important::

   **The server decides which authentication methods PMIx uses.**
   PMIx's checks on a connection are a triage, and the first of them is
   the credential, checked by a security (``psec``) component. A server
   accepts only the components it has loaded, and passes that list to
   the clients it starts (through
   :ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>`) and
   to tools (in its rendezvous file). A process cannot use a method its
   server does not offer, so the server's settings are the ones that
   count.

   The components a server loads are chosen when it starts, by the
   ``psec`` MCA parameter of the PMIx installation it runs from:

   * **The host** sets it before calling ``PMIx_server_init`` - for
     example, ``PMIX_MCA_psec=munge`` in its environment - when its
     deployment calls for particular methods, such as ``munge`` for
     peers on other nodes.
   * **A system administrator** sets it in that installation's
     ``<prefix>/etc/pmix-mca-params-override.conf``, whose settings take
     precedence over the environment's - for every process that reads
     the file. A process whose environment points its library at other
     parameter files, or at none, does not read it (see
     :ref:`label-sysadmin-authentication`), so the file sets the policy
     for servers the administrator starts, or whose environment the
     administrator controls. A component the installation is built
     without, or whose plugin is removed, is unavailable to every
     process of that installation.
   * **A server started by a client** - a nested server - inherits
     ``PMIX_SECURITY_MODE`` from the environment its own server gave the
     client, and takes it as its ``psec`` setting unless ``PMIX_MCA_psec``
     is set. It offers its own peers no more than its parent server
     offered. A server started by a process that has itself called
     ``PMIx_Init`` or ``PMIx_tool_init`` inherits no ``PMIX_SECURITY_MODE``
     - that init took it out of the environment - and is not narrowed.

   Each PMIx installation reads only the parameter files under its own
   prefix, so the settings belong in the installation the host's server
   runs from. See :ref:`label-sysadmin-authentication`.


Accepting connections
---------------------

Every client and tool connection that passes PMIx's checks reaches the
host through ``client_connected2`` or ``tool_connected2``. The full
rules are on :doc:`connections`; in brief:

* **Answer every one, once, within** ``PMIX_TIMEOUT``. The up-call
  carries the number of seconds the host is asked to answer within. A
  host that cannot decide in time should answer no; the server refuses
  the process itself a little after that time.
* **You decide on one tool of a name at a time.** A slow answer delays
  the tools of that name waiting behind it, and their time waiting
  counts against their own wait.
* **Be ready to name a tool you launched.** A tool you started, whose
  namespace and rank are already in use - several tools started by one
  job script, say - connects again as a tool that needs an identity,
  and you must give it a namespace of its own.
* **Handle** ``PMIX_ERR_LOST_CONNECTION`` **for processes you
  accepted.** A process you said yes to may never become connected -
  your answer came too late, or it went away first. You are then sent
  this event naming it, as for any process that leaves without
  finalizing. Register a handler for it.
* **Expect a process that execs to be lost.** A client or tool of this
  release closes its connection when it execs: you are sent
  ``PMIX_ERR_LOST_CONNECTION`` for it at once, whether the process runs
  on or not.

Do not call :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`
after a failed ``PMIx_server_init``.


Writing the up-calls
--------------------

The rules below apply to every function in
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`.

* **Leave out what you do not support.** A ``NULL`` entry makes the
  library return ``PMIX_ERR_NOT_SUPPORTED`` to the requester. For some
  operations the library then does the work itself; see
  :doc:`operations`.
* **Return quickly.** Up-calls run on the PMIx progress thread. Start
  the work, return, and report the result later through the callback
  you were given.
* **Return the right code.**

  - ``PMIX_SUCCESS``: you accepted the request and **will** call the
    callback.
  - ``PMIX_OPERATION_SUCCEEDED``: the work is already done and you
    **will not** call the callback. Not allowed from ``spawn``,
    ``lookup``, ``direct_modex``, ``get_credential`` or
    ``validate_credential``: their results can only travel through the
    callback.
  - Any error: the request failed and you will not call the callback.

* **Do not make a blocking PMIx call from an up-call** or from any
  other code running on the progress thread. It waits for itself and
  returns ``PMIX_ERR_WOULD_BLOCK``. Use the non-blocking form, or call
  from a thread of your own.
* **Do not free what the library passes you,** except the ``data`` blob
  of ``fence_nb``, which becomes yours and which you must ``free()``.
  What you pass back through a callback stays yours; you may release it
  once the callback returns, or through the release function the
  callback takes. See
  :ref:`pmix_callback_functions(5) <man5-pmix_callback_functions>`.
* **Use the requester's identity.** Every up-call made for a client or
  tool carries the requester's ``PMIX_USERID`` and ``PMIX_GRPID``. Use
  them to decide whether to perform the request; see
  :doc:`authorization`.


Before launching a process
--------------------------

Do these, in this order, before you start any process that will call
``PMIx_Init``:

#. **Register the job** with
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`.

   - Register *every* job that takes part in a collective with a local
     process, including jobs that have no process on this node.
   - Give the number of local processes. The library uses it to know
     when the local part of a collective is complete.
   - Give the job-level information applications will ask for. PMIx
     serves it to them without asking you again. This is what makes
     "instant on" startup possible.
   - Give the job's owner (``PMIX_USERID``, ``PMIX_GRPID``) and the
     users and groups the owner allowed (``PMIX_ACCESS_PERMISSIONS``).
     Do this on every node that hosts part of the job. See
     :doc:`authorization`.

#. **Register each local client** with
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   exactly once per rank. The uid and gid you give are the identity the
   client must prove when it connects. A client of a job you have not
   registered, or of a rank you have not registered, is refused.

#. **Set up the client's environment** with
   :ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>`. This
   is required. It gives the client what it needs to find and connect
   to the server.

#. **If you used application setup,** deliver its result on every node
   with
   :ref:`PMIx_server_setup_local_support(3) <man3-PMIx_server_setup_local_support>`.
   The data comes from
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   which a launcher calls once for the job.

Do not use ``PMIx_Store_internal`` to change a job's data. To update a
registered job, register it again with a negative number of local
processes; see
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`.


While processes run
-------------------

* **Report events about your jobs.** You must report events concerning
  a registered job - a process or job terminating, for example -
  whether or not anything has registered for them. ``register_events``
  only tells you which *system* events to forward as well.
* **Report a connected process that dies.** When a process that called
  :ref:`PMIx_Connect(3) <man3-PMIx_Connect>` terminates without
  disconnecting, generate ``PMIX_ERR_PROC_TERM_WO_SYNC`` for the
  processes connected to it.
* **Deliver events where they are needed.** An event aimed at
  particular processes must reach every server where they run. Name the
  affected processes with ``PMIX_EVENT_AFFECTED_PROC`` or
  ``PMIX_EVENT_AFFECTED_PROCS``. See
  :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`.
* **Forward output.** Deliver output to the servers that asked for it
  with :ref:`PMIx_server_IOF_deliver(3) <man3-PMIx_server_IOF_deliver>`.
* **Fetch data from other nodes.** Answer ``direct_modex`` up-calls by
  asking the server that holds the data, using
  :ref:`PMIx_server_dmodex_request2(3) <man3-PMIx_server_dmodex_request2>`
  so that the requester's identity goes with the request.
* **Keep process sets and groups consistent.** If you define process
  sets, every server must agree on their membership, and their names
  must not collide. See
  :ref:`PMIx_server_define_process_set(3) <man3-PMIx_server_define_process_set>`.


Shutting down
-------------

* **Finish each client.** The ``client_finalized`` up-call tells you a
  client called ``PMIx_Finalize``. Release your state for it, then call
  the callback to let the client go. Do not deregister it there: the
  process may call ``PMIx_Init`` again.
* **Optionally, deregister each client** with
  :ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`
  once its process has terminated. A client that finalized is still a
  member of its job, and collectives that name it keep waiting for it,
  until you do. PRRTE deregisters each local process when it has
  terminated.
* **Deregister each job** with
  :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`
  once nothing on this node needs it. That removes every client still
  registered in it.
* **Deregister each user** you registered with
  :ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
  once nothing you run or serve belongs to that user.
* **Finalize the server** with
  :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`, and not
  from within an up-call or callback.

For the full life cycle of a client, including clients that initialize
and finalize more than once, see :doc:`/how-things-work/init-finalize`.
