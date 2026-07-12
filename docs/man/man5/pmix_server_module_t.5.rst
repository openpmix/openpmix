.. _man5-pmix_server_module_t:

pmix_server_module_t
====================

.. include_body

`pmix_server_module_t` |mdash| The set of callback functions a host
environment provides to the PMIx server library.

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_server.h>

   typedef struct pmix_server_module_4_0_0_t {
       /* v1x interfaces */
       pmix_server_client_connected_fn_t   client_connected; // DEPRECATED
       pmix_server_client_finalized_fn_t   client_finalized;
       pmix_server_abort_fn_t              abort;
       pmix_server_fencenb_fn_t            fence_nb;
       pmix_server_dmodex_req_fn_t         direct_modex;
       pmix_server_publish_fn_t            publish;
       pmix_server_lookup_fn_t             lookup;
       pmix_server_unpublish_fn_t          unpublish;
       pmix_server_spawn_fn_t              spawn;
       pmix_server_connect_fn_t            connect;
       pmix_server_disconnect_fn_t         disconnect;
       pmix_server_register_events_fn_t    register_events;
       pmix_server_deregister_events_fn_t  deregister_events;
       pmix_server_listener_fn_t           listener;
       /* v2x interfaces */
       pmix_server_notify_event_fn_t       notify_event;
       pmix_server_query_fn_t              query;
       pmix_server_tool_connection_fn_t    tool_connected; // DEPRECATED
       pmix_server_log_fn_t                log;            // DEPRECATED
       pmix_server_alloc_fn_t              allocate;
       pmix_server_job_control_fn_t        job_control;
       pmix_server_monitor_fn_t            monitor;
       /* v3x interfaces */
       pmix_server_get_cred_fn_t           get_credential;
       pmix_server_validate_cred_fn_t      validate_credential;
       pmix_server_iof_fn_t                iof_pull;
       pmix_server_stdin_fn_t              push_stdin;
       /* v4x interfaces */
       pmix_server_grp_fn_t                group;
       pmix_server_fabric_fn_t             fabric;
       /* v6x interfaces */
       pmix_server_client_connected2_fn_t  client_connected2;
       pmix_server_tool_connection2_fn_t   tool_connected2;
       pmix_server_log2_fn_t               log2;
       /* pending interfaces */
       pmix_server_session_control_fn_t    session_control;
       pmix_server_resource_block_fn_t     resource_block;

   } pmix_server_module_t;


DESCRIPTION
-----------

The `pmix_server_module_t` structure is the primary integration point between
the PMIx server library and its host environment |mdash| a resource manager,
launcher, or other system-management-stack (SMS) element. It is a table of
function pointers, one per PMIx server operation, that the host implements and
passes by pointer to :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`.

PMIx uses a "function-shipping" model: each PMIx client request that requires
host action is mirrored by an entry in this structure. When the PMIx server
library receives such a request from a local client (or tool), it invokes the
corresponding callback in the host's module, handing the host the parameters
needed to service the request. This lets a resource manager implement the
server role without any knowledge of PMIx internals.

**Unimplemented callbacks may be left NULL.** A host is not required to support
every operation. Any function the host does not provide is indicated by a NULL
pointer in the structure; the library detects the NULL entry and returns
``PMIX_ERR_NOT_SUPPORTED`` to the requesting client for that operation rather
than dereferencing the pointer (see, for example, ``pmix_server_publish`` in
``src/server/pmix_server_ops.c``, which returns ``PMIX_ERR_NOT_SUPPORTED`` when
``NULL == pmix_host_server.publish``). This behavior is documented at the top of
``include/pmix_server.h``: "Any functions not supported by the RM can be
indicated by a NULL for the function pointer. Client calls to such functions
will have a 'not supported' error returned."

**Callbacks are asynchronous.** For performance and scalability, the host is
required to return from every callback as quickly as possible, performing the
actual work asynchronously. Nearly all callbacks are therefore non-blocking:
they accept a completion callback function (``cbfunc``) and an opaque
``cbdata`` pointer, and the host is expected to invoke ``cbfunc`` with the
result once the operation completes. The type of the completion function varies
by operation (for example, ``pmix_op_cbfunc_t`` for simple status returns,
``pmix_modex_cbfunc_t`` for fence/modex data, ``pmix_info_cbfunc_t`` for query,
allocate, job-control, monitor, group, fabric, and session operations, and so
on).

**Return-value contract.** The value a callback returns tells the library how
completion will be signaled:

* ``PMIX_SUCCESS`` |mdash| the request has been accepted and is being processed
  asynchronously. The host **will** invoke the supplied ``cbfunc`` later with
  the final result.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| the operation completed immediately and
  successfully. The host **must not** invoke ``cbfunc``; the library treats the
  operation as already finished.
* Any other (error) value |mdash| the request failed and the host **will not**
  invoke ``cbfunc``. The library reports the error to the client directly and
  releases any state it was holding for the request.

**Data ownership.** All data passed into a host callback is owned by the PMIx
server library and must not be freed by the host. Conversely, data the host
returns through a completion callback is owned by the host, which may release it
once the callback returns.


ABI STABILITY
-------------

The `pmix_server_module_t` structure is consumed by host environments that may
initialize it using **positional** (non-designated) initializers. Consequently
the order of fields is a binary-compatibility contract and is **frozen**:

* Existing fields are never removed and never reordered.
* New callbacks are only ever **appended** at the end of the structure. The
  ``/* v1x interfaces */`` ... ``/* pending interfaces */`` group comments record
  the release series in which each block of members was added.
* When a callback's signature must change, the original field is **retained in
  place** (marked ``// DEPRECATED``) and a replacement is appended at the end
  with a numeric suffix on its name.

The structure currently carries three deprecation pairs. In each case the
library prefers the newer member and falls back to the deprecated one only if
the newer member is NULL:

* ``client_connected`` |rarrow| ``client_connected2``. The original
  ``pmix_server_client_connected_fn_t`` takes only ``proc``, ``server_object``,
  ``cbfunc``, and ``cbdata``. The replacement
  ``pmix_server_client_connected2_fn_t`` inserts an ``info[]``/``ninfo`` array so
  additional information can be passed about the connecting client.
* ``tool_connected`` |rarrow| ``tool_connected2``. The original
  ``pmix_server_tool_connection_fn_t`` returns ``void``; the replacement
  ``pmix_server_tool_connection2_fn_t`` has an otherwise identical signature but
  returns a ``pmix_status_t`` so the host can synchronously reject a tool
  connection request.
* ``log`` |rarrow| ``log2``. The original ``pmix_server_log_fn_t`` returns
  ``void``; the replacement ``pmix_server_log2_fn_t`` has an identical parameter
  list but returns a ``pmix_status_t`` so the host can indicate synchronously
  whether the log request was accepted.


CALLBACK FUNCTIONS
------------------

The members are described below in structure order.

client_connected
^^^^^^^^^^^^^^^^^

``client_connected`` (``pmix_server_client_connected_fn_t``) |mdash|
**DEPRECATED**; use ``client_connected2``. Invoked when a registered client
connects to the local server. The client is held blocked until the host invokes
``cbfunc``, allowing the library to release it. This original form conveys only
the client's ``proc`` and ``server_object``.

client_finalized
^^^^^^^^^^^^^^^^^

``client_finalized`` (``pmix_server_client_finalized_fn_t``) |mdash| Notifies the
host that a local client has called `PMIx_Finalize`. The client is held blocked
until the host invokes ``cbfunc``. The host should release any per-client state
it associated with the process via ``PMIx_server_register_client``.

abort
^^^^^

``abort`` (``pmix_server_abort_fn_t``) |mdash| Services
:ref:`PMIx_Abort(3) <man3-PMIx_Abort>`. A local client has requested that a set
of processes be terminated; the ``procs`` array names the targets, or a NULL
array means all processes in the caller's namespace. The requesting client is
held blocked until the host invokes ``cbfunc``. The host performs the requested
termination and reports the result.

fence_nb
^^^^^^^^

``fence_nb`` (``pmix_server_fencenb_fn_t``) |mdash| Services
:ref:`PMIx_Fence(3) <man3-PMIx_Fence>` and its non-blocking form. Called once all
local participants of a fence have contributed; the host collectively exchanges
the supplied ``data`` blob among all servers hosting participants and returns the
aggregated result through a ``pmix_modex_cbfunc_t``. A NULL ``data`` means the
local processes had nothing to contribute. Directives in the ``info`` array
steer the collective and are optional unless the mandatory flag is set.

The ``info`` array may include ``PMIX_LOCAL_COLLECTIVE_STATUS``
(pmix_status_t), by which the PMIx server library reports to the host the status
of the local portion of the collective |mdash| for example, an error detected
among the local participants. A host that receives a failing status should
propagate it into the collective result rather than proceeding with the data
exchange. The library may likewise supply ``PMIX_LOCAL_PARTICIPANTS``
(pmix_data_array_t*), an array of ``pmix_proc_t`` identifying the local processes
that contributed to the collective. These attributes apply equally to the other
collective module functions below (``connect``, ``disconnect``, and ``group``).

direct_modex
^^^^^^^^^^^^

``direct_modex`` (``pmix_server_dmodex_req_fn_t``) |mdash| Requests that the host
contact the remote server hosting a given process and retrieve that process's
modex blob on demand (used when a :ref:`PMIx_Get(3) <man3-PMIx_Get>` cannot be
satisfied from local data). The blob is returned through a
``pmix_modex_cbfunc_t``. A timeout directive may be supplied to bound the wait.
The request may carry a ``PMIX_REQUIRED_KEY`` (char*) naming the specific key the
requester is waiting on, allowing the host to defer its response until that key
has been posted by the target process.

publish
^^^^^^^

``publish`` (``pmix_server_publish_fn_t``) |mdash| Services
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>`. Stores the provided key/value data
in the host's data store subject to the requested range and persistence
directives, recording the publisher's identity for later lookup. Completion is
reported through a ``pmix_op_cbfunc_t``.

lookup
^^^^^^

``lookup`` (``pmix_server_lookup_fn_t``) |mdash| Services
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`. Retrieves previously published data
for a NULL-terminated array of string keys, honoring any wait/timeout directives,
and returns the results through a ``pmix_lookup_cbfunc_t``.

unpublish
^^^^^^^^^

``unpublish`` (``pmix_server_unpublish_fn_t``) |mdash| Services
:ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`. Deletes previously published data
matching a NULL-terminated array of keys (within any specified range) and reports
completion through a ``pmix_op_cbfunc_t``.

spawn
^^^^^

``spawn`` (``pmix_server_spawn_fn_t``) |mdash| Services
:ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`. Launches the given array of applications.
A failure to start any process causes the entire request to be terminated and an
error returned. The namespace of the spawned job is returned through a
``pmix_spawn_cbfunc_t``. The job-level information accompanying the request may
include ``PMIX_REQUESTOR_IS_TOOL`` or ``PMIX_REQUESTOR_IS_CLIENT`` (bool),
indicating whether the process that issued the spawn request is a tool or a
client, respectively; a host may use this to apply different policies to
tool-initiated launches.

connect
^^^^^^^

``connect`` (``pmix_server_connect_fn_t``) |mdash| Services
:ref:`PMIx_Connect(3) <man3-PMIx_Connect>`. Records the specified processes as
"connected" so the host treats the failure of any of them as a reportable event.
This is a client-side collective, so the callback fires once all participants
have contributed; completion is reported through a ``pmix_op_cbfunc_t``. As with
``fence_nb``, the ``info`` array may carry ``PMIX_LOCAL_COLLECTIVE_STATUS`` and
``PMIX_LOCAL_PARTICIPANTS`` reporting the status and membership of the local
portion of the collective.

disconnect
^^^^^^^^^^

``disconnect`` (``pmix_server_disconnect_fn_t``) |mdash| Services
:ref:`PMIx_Disconnect(3) <man3-PMIx_Disconnect>`. Reverses a prior ``connect`` of
the same set of processes; an error is returned if the set was not previously
connected. Completion is reported through a ``pmix_op_cbfunc_t``. As with
``fence_nb``, the ``info`` array may carry ``PMIX_LOCAL_COLLECTIVE_STATUS`` and
``PMIX_LOCAL_PARTICIPANTS`` reporting the status and membership of the local
portion of the collective.

register_events
^^^^^^^^^^^^^^^

``register_events`` (``pmix_server_register_events_fn_t``) |mdash| Part of the
support for :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`.
Tells the host that the library wishes to receive notification of the specified
(typically environmental) event codes; the host translates its internal codes to
the corresponding PMIx codes when it later notifies. Completion is reported
through a ``pmix_op_cbfunc_t``.

deregister_events
^^^^^^^^^^^^^^^^^

``deregister_events`` (``pmix_server_deregister_events_fn_t``) |mdash| The
companion to ``register_events`` (see
:ref:`PMIx_Deregister_event_handler(3) <man3-PMIx_Deregister_event_handler>`).
Cancels a prior registration for the specified event codes. The host remains
obligated to report job-related events regardless.

listener
^^^^^^^^

``listener`` (``pmix_server_listener_fn_t``) |mdash| Optionally lets the host own
the rendezvous listening socket. The host monitors the descriptor, accepts
incoming client connections, and passes each accepted socket to the provided
``pmix_connection_cbfunc_t``. Leaving this member NULL causes the library to
spawn its own internal listener thread.

notify_event
^^^^^^^^^^^^

``notify_event`` (``pmix_server_notify_event_fn_t``) |mdash| Services
:ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>` when an event generated by
the server or one of its local clients must be delivered beyond the local node.
The host propagates the event (identified by ``code``, ``source``, and ``range``)
and reports completion through a ``pmix_op_cbfunc_t``.

query
^^^^^

``query`` (``pmix_server_query_fn_t``) |mdash| Services
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`. The host answers an array of
``pmix_query_t`` requests on behalf of the identified process and returns the
results through a ``pmix_info_cbfunc_t``.

tool_connected
^^^^^^^^^^^^^^

``tool_connected`` (``pmix_server_tool_connection_fn_t``) |mdash| **DEPRECATED**;
use ``tool_connected2``. Invoked when a tool connects and needs a namespace/rank
assignment. The host returns the assigned identifier through a
``pmix_tool_connection_cbfunc_t``. This original form returns ``void`` and so
cannot synchronously reject the connection.

log
^^^

``log`` (``pmix_server_log_fn_t``) |mdash| **DEPRECATED**; use ``log2``. Services
:ref:`PMIx_Log(3) <man3-PMIx_Log>` on behalf of a client. The host must not call
`PMIx_Log` from within this callback (doing so would create an infinite loop);
instead it forwards the data to a gateway/logging channel or returns a
not-supported error. Completion is reported through a ``pmix_op_cbfunc_t``. This
original form returns ``void``.

allocate
^^^^^^^^

``allocate`` (``pmix_server_alloc_fn_t``) |mdash| Services
:ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`. The host acts
on the requested allocation modification (identified by a
``pmix_alloc_directive_t``) and returns any results through a
``pmix_info_cbfunc_t``.

job_control
^^^^^^^^^^^

``job_control`` (``pmix_server_job_control_fn_t``) |mdash| Services
:ref:`PMIx_Job_control(3) <man3-PMIx_Job_control>`. The host executes the
requested control action (e.g., pause, resume, signal, terminate) against the
target processes and returns results through a ``pmix_info_cbfunc_t``.

monitor
^^^^^^^

``monitor`` (``pmix_server_monitor_fn_t``) |mdash| Services
:ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`. The host arranges the
requested monitoring (e.g., heartbeat or file-based) of the requestor, raising
the specified error status on failure, and returns results through a
``pmix_info_cbfunc_t``.

get_credential
^^^^^^^^^^^^^^

``get_credential`` (``pmix_server_get_cred_fn_t``) |mdash| Services
:ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>`. The host requests a
security credential from the SMS on behalf of the identified process and returns
it through a ``pmix_credential_cbfunc_t``.

validate_credential
^^^^^^^^^^^^^^^^^^^

``validate_credential`` (``pmix_server_validate_cred_fn_t``) |mdash| Services
:ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>`. The host asks
the SMS to validate the supplied credential and returns the result through a
``pmix_validation_cbfunc_t``.

iof_pull
^^^^^^^^

``iof_pull`` (``pmix_server_iof_fn_t``) |mdash| Services
:ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`. Registers the local server to
receive the specified IO channels (a ``pmix_iof_channel_t`` bitmask) forwarded
from a set of source processes; the ``PMIX_IOF_STOP`` directive instead removes
the server from the distribution list. Note that ``stdin`` cannot be pulled with
this call. Completion is reported through a ``pmix_op_cbfunc_t``.

push_stdin
^^^^^^^^^^

``push_stdin`` (``pmix_server_stdin_fn_t``) |mdash| Services
:ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`. Passes a ``stdin`` byte object from
a source process to the host for delivery to the named target processes (a
WILDCARD rank targets all processes in a namespace). Completion is reported
through a ``pmix_op_cbfunc_t``.

group
^^^^^

``group`` (``pmix_server_grp_fn_t``) |mdash| Services
:ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>` and the related group
operations. The ``op`` argument (a ``pmix_group_operation_t``) selects construct,
destruct, or cancel of the named group across its member processes; directives
may request, for example, assignment of a group context ID. Results are returned
through a ``pmix_info_cbfunc_t``. As with ``fence_nb``, the ``info`` array may
carry ``PMIX_LOCAL_COLLECTIVE_STATUS`` and ``PMIX_LOCAL_PARTICIPANTS`` reporting
the status and membership of the local portion of the collective.

fabric
^^^^^^

``fabric`` (``pmix_server_fabric_fn_t``) |mdash| Services
:ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>` and related fabric
operations. The host retrieves fabric-related information from the server
supporting the system scheduler, per the ``pmix_fabric_operation_t`` op, and
returns it through a ``pmix_info_cbfunc_t``.

client_connected2
^^^^^^^^^^^^^^^^^

``client_connected2`` (``pmix_server_client_connected2_fn_t``) |mdash| Replaces
the deprecated ``client_connected``. Invoked when a registered client connects;
the library prefers this member and only falls back to ``client_connected`` when
this is NULL. Adds an ``info[]``/``ninfo`` array carrying additional information
about the connecting client. The client is held blocked until the host invokes
``cbfunc``.

tool_connected2
^^^^^^^^^^^^^^^

``tool_connected2`` (``pmix_server_tool_connection2_fn_t``) |mdash| Replaces the
deprecated ``tool_connected``. Invoked when a tool connects and needs a
namespace/rank assignment; the library prefers this member over
``tool_connected``. Unlike the original, it returns a ``pmix_status_t`` so the
host can synchronously reject the connection. The assigned identifier is returned
through a ``pmix_tool_connection_cbfunc_t``.

log2
^^^^

``log2`` (``pmix_server_log2_fn_t``) |mdash| Replaces the deprecated ``log`` and
services :ref:`PMIx_Log(3) <man3-PMIx_Log>`; the library prefers this member and
falls back to ``log`` only when it is NULL. Identical in parameters to ``log``
but returns a ``pmix_status_t``. As with ``log``, the host must not call
`PMIx_Log` from within this callback.

session_control
^^^^^^^^^^^^^^^

``session_control`` (``pmix_server_session_control_fn_t``) |mdash| Services
:ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>`. The host executes the
requested control operation against the identified session (``sessionID``) and
returns results through a ``pmix_info_cbfunc_t``.

resource_block
^^^^^^^^^^^^^^

``resource_block`` (``pmix_server_resource_block_fn_t``) |mdash| Services
:ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`. The host defines,
deletes, extends, or removes resources from a named resource block per the
``pmix_resource_block_directive_t`` and the supplied array of
``pmix_resource_unit_t`` units. Completion is reported through a
``pmix_op_cbfunc_t``.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`PMIx_Connect(3) <man3-PMIx_Connect>`,
   :ref:`PMIx_Disconnect(3) <man3-PMIx_Disconnect>`,
   :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
   :ref:`PMIx_Deregister_event_handler(3) <man3-PMIx_Deregister_event_handler>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Log(3) <man3-PMIx_Log>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Job_control(3) <man3-PMIx_Job_control>`,
   :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`,
   :ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>`,
   :ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>`,
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`,
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>`,
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
