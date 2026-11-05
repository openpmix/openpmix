Schedulers and PMIx v7
======================

For developers of system schedulers (workload managers) that use PMIx:
a scheduler that embeds the PMIx server library and declares itself the
system scheduler (``PMIX_SERVER_SCHEDULER``), so that runtime
environments - a DVM such as PRRTE's ``prte``, say - attach to it and
pass on their users' requests.

A scheduler is a host environment, so everything on :doc:`hosts`
applies to it. This page collects what matters most to a scheduler, and
adds the parts only a scheduler meets: being found by the runtimes that
attach to it, accepting them, and judging the requests they relay. How
the scheduler integration works is in
:doc:`/how-things-work/schedulers/index`.


How a scheduler fits
--------------------

A runtime environment attaches to the scheduler as a tool, with
``PMIX_CONNECT_TO_SCHEDULER``. It then uses that connection for two
things:

* **its own business with the scheduler** - for example, telling it
  that a session has ended;
* **its users' requests**, which it relays: an allocation request
  (``PMIx_Allocation_request``, the ``allocate`` up-call), a session
  control request (``PMIx_Session_control``, ``session_control``), a
  resource block (``PMIx_Resource_block``, ``resource_block``), a
  query, or a fabric request.

The second is where v7 changes the most: what a request tells the
scheduler about who made it. See `Who is asking`_.


Runtimes connect only on the scheduler's node
---------------------------------------------

**What changed.** PMIx messaging is node-local. A tool - and an
attaching runtime is a tool to the scheduler - connects only to a PMIx
server on its own node. Connections from other nodes are gone, and a
server that asks for them (``PMIX_SERVER_REMOTE_CONNECTIONS``) fails
to start.

**What it means.** A runtime attaches to a scheduler server on the node
it runs on. It looks for ``pmix.sched.<hostname>``, named for its own
node. A scheduler that serves runtimes on other nodes runs a PMIx
server on each of those nodes - an agent - and carries requests between
its agents and its core over its own transport, as resource managers
always have for their clients.


Being found
-----------

**Ask for tool support** (``PMIX_SERVER_TOOL_SUPPORT``) at
``PMIx_server_init``. Without it, no runtime can attach.

**Where the scheduler's rendezvous file goes.** The scheduler writes
``pmix.sched.<hostname>``:

* in the system temporary directory, if the host names one
  (``PMIX_SYSTEM_TMPDIR``);
* else in ``/run/pmix``, when the scheduler runs as root;
* else in its own user's directory, ``<tmpdir>/pmix-user.<uid>/``.

**Where a runtime looks.** A runtime looking for the scheduler tries, in
order, the system temporary directory it was given, ``/run/pmix``,
``/var/run/pmix``, root's and its own user's directories, and the
directories of the users its ``ptl_base_server_access`` parameter
names. With legacy peers on, it also looks in the system temporary
directory, where a scheduler of an earlier release writes its file.

**What a runtime believes.** It uses a file only if it was written by
root, by its own user, or by a user ``ptl_base_server_access`` names -
and only if the kernel confirms that the server behind the socket runs
as the user who wrote the file.

**So:**

* a scheduler running as root, or as each runtime's own user, is found
  with no configuration;
* a scheduler running as a service account - ``sched``, say - is found
  only by runtimes whose PMIx parameters name that account:
  ``ptl_base_server_access = users=sched`` in the parameter file of
  each account a runtime runs as.

**Handing out the address yourself.** A TCP address for a v7 server is
refused (``PMIX_ERR_LOCAL_URI_REQUIRED``). ``PMIX_TCP_REPORT_URI`` set
to ``-`` or ``+`` now reports the local socket's address, which is the
one to give a runtime; ``tcp-`` and ``tcp+`` report the TCP address,
for runtimes of earlier releases.

**Use a short temporary directory.** The local socket lives under it,
and a socket path is limited to about 100 characters.


Accepting runtimes
------------------

**The local socket is owner-only.** The server takes, without being
told to, tools of root, of the scheduler's own user, and of the user
the host names as ``PMIX_USERID`` at ``PMIx_server_init``. Runtimes
usually run as other accounts - a user's own DVM, or a privileged
"backbone" DVM. To take them:

* pass ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` at ``PMIx_server_init``;
* and widen the socket to them: ``PMIX_SOCKET_MODE``, with
  ``PMIX_USERID`` and ``PMIX_GRPID`` naming the socket's owner and
  group - for example, a group the runtimes' accounts belong to, with
  mode 0660. Connecting to a socket takes write permission on it.

**Decide in** ``tool_connected2``. It receives the runtime's verified
user and group, the authentication method, the kernel's pid for the
process, and ``PMIX_TIMEOUT``. Check:

* that this user may use the scheduler at all;
* that what the runtime asks to be is something that user may be. The
  roles a tool declares at connection (``PMIX_LAUNCHER``, for instance)
  are its own claim.

Answer once, within the timeout. A runtime may arrive with no namespace:
assign one, and keep the names you assign unique. See
:doc:`/hosts/connections`.

**Handle** ``PMIX_ERR_LOST_CONNECTION``. It is raised for a runtime
that leaves without finalizing, so the scheduler can act on the
sessions and requests it held.

**Runtimes of earlier releases.** A runtime linked to a PMIx release
before v7 connects over TCP, on loopback only, and only to a server set
up to take legacy peers: PMIx configured with
``--enable-legacy-peers``, and either the host's
``PMIX_SERVER_LEGACY_PEERS`` or the site's ``ptl_base_legacy_peers``.
Without both, it is refused.


Who is asking
-------------

**What changed.**

* ``PMIX_USERID`` and ``PMIX_GRPID`` in an up-call always describe the
  process connected to the scheduler - the runtime - as the kernel
  reported them when it connected. ``PMIX_REQUESTOR_PID`` comes with
  them, on the local socket. Nothing the runtime sends replaces them.
  ``PMIX_REQUESTOR_IS_TOOL`` and ``PMIX_REQUESTOR_IS_CLIENT`` likewise
  describe the runtime.
* The process that first made the request, as the runtime states it,
  arrives beside them: ``PMIX_ORIGIN_USERID``, ``PMIX_ORIGIN_GRPID``,
  ``PMIX_ORIGIN_PID``, ``PMIX_ORIGIN_IS_TOOL`` and
  ``PMIX_ORIGIN_IS_CLIENT``, with ``PMIX_REQUESTOR`` for its process
  ID. PMIx passes these through untouched, and refuses a request that
  carries any of them more than once.
* A group the requester asks the work to be charged to arrives as
  ``PMIX_REQUESTED_GRPID``. A requester's own ``PMIX_GRPID`` is turned
  into one; a group name is resolved to a number; two are refused. PMIx
  does not check whether the requester belongs to the group.
* ``PMIX_INFO_RELAYED`` and ``PMIx_Info_relayed()`` are gone.
* ``PMIX_CAP_ORIGIN_ID`` in ``pmix_version.h`` says the library behaves
  this way.

**What it means.** A scheduler that has not been changed judges every
relayed request as the runtime's own. It never acts as a user a runtime
merely names - but it may refuse, or charge to the runtime's account,
work its users asked for.

**What to do.**

#. **Authorize on** ``PMIX_USERID`` **and** ``PMIX_GRPID``. They are the
   runtime you are talking to.
#. **Decide which runtimes may speak for others**, from those verified
   ids, and from the pid if you want to be exact. Which accounts qualify
   is the scheduler's policy - in a three-level setup of user DVM,
   backbone DVM and scheduler, typically the backbone's account. PRRTE's
   rule, as an example: it believes a claim when the claimed user is the
   relay's own, when the relay runs as root or as the DVM's user, or when
   the relay's uid is listed in its ``prte_pmix_relay_uids`` parameter.
#. **For a runtime you trust, attribute the work to the**
   ``PMIX_ORIGIN_*`` **identity.** For any other, treat the request as
   the runtime's own.
#. **Check** ``PMIX_REQUESTED_GRPID`` against the user you attributed
   the work to.
#. **If the scheduler passes a request on** to another server, copy the
   identity it attributed the work to into ``PMIX_ORIGIN_*`` - at the
   first hop, the up-call's own ``PMIX_USERID``, ``PMIX_GRPID`` and
   ``PMIX_REQUESTOR_PID`` - and carry a requested group on as
   ``PMIX_REQUESTED_GRPID``.

See "Requests that pass through more than one server" in
:doc:`/hosts/authorization`.


Users' groups
-------------

PMIx never looks groups up. A process's groups come from the kernel for
a connection on the local socket; for anything else, PMIx uses the
groups the host gave, else the single gid the request is made under.

So a scheduler that checks ``PMIX_REQUESTED_GRPID`` needs the user's
groups from its own source. Look them up on the scheduler's own thread,
not in an up-call it must answer quickly.

A scheduler that applies PMIx's access rule to its own objects
(``pmix_server_access_check()``) gives the groups of users who own no
job registered with it through ``PMIx_server_register_resources``, with
``PMIX_USERID`` and ``PMIX_USER_GRPIDS``. See
:doc:`/hosts/access-control`.


The up-calls
------------

* **Every up-call made for a runtime** carries ``PMIX_USERID``,
  ``PMIX_GRPID`` and ``PMIX_REQUESTOR_PID``, and the
  ``PMIX_ORIGIN_*`` claim when the runtime sends one.
* ``allocate``, ``session_control`` **and** ``resource_block``: PMIx
  passes the request up with that identity. The
  scheduler decides whether the requester may obtain, change or release
  the resources or session concerned. A ``NULL`` entry answers
  ``PMIX_ERR_NOT_SUPPORTED``.
* **Returning** ``PMIX_OPERATION_SUCCEEDED`` from ``spawn``,
  ``lookup``, ``direct_modex``, ``get_credential`` or
  ``validate_credential`` is treated as an error: each has a result that
  only the callback can deliver.
* **The library no longer relays on its own.** A spawn or query it
  cannot serve goes to your up-call, and is refused if there is none.

See :doc:`/hosts/operations`.


Building
--------

* **Build against the PMIx major release the scheduler runs with.** The
  library is given only a pointer to the host's
  ``pmix_server_module_t``, and cannot tell which release's definition
  of it the host was built with.
* **Test the capability flags** in ``pmix_version.h`` from
  ``configure`` - ``PMIX_CAP_ORIGIN_ID`` and ``PMIX_CAP_REQUESTER_ID``
  in particular - if the scheduler must also build against earlier
  releases.
