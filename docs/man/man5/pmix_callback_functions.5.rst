.. _man5-pmix_callback_functions:

PMIx Callback Functions
=======================

.. include_body

The PMIx callback function signatures |mdash| the function-pointer types
through which PMIx returns the results of non-blocking operations and
delivers events.


DESCRIPTION
-----------

Most PMIx operations are available in a non-blocking form. A non-blocking
API accepts a callback function of one of the types documented here, together
with an opaque ``void *cbdata`` pointer. The API returns immediately once the
request has been accepted; the library later invokes the supplied callback on
its internal progress thread when the operation completes, passing the
``cbdata`` pointer back unmodified so the caller can correlate the response
with its originating request. A leading ``pmix_status_t status`` argument (when
present) reports the outcome: a value of ``PMIX_SUCCESS`` indicates the
operation succeeded, while any other value is an error constant describing the
failure. See :ref:`pmix_status_t(5) <man5-pmix_status_t>`.

**Shared ownership contract.** Data passed *to* a callback is owned by the
PMIx library. Unless otherwise stated, that data is valid only for the
duration of the call and is released by the library as soon as the callback
returns; a callee that needs to retain any of it must copy it before
returning. Some callbacks additionally receive a
:ref:`pmix_release_cbfunc_t <man5-pmix_release_cbfunc_t>` ``release_fn`` and a
matching ``release_cbdata``. In that case the callee is permitted to retain
the provided data (without copying) and must invoke ``release_fn(release_cbdata)``
when it is finished with it, so the library can free the underlying storage.
If ``release_fn`` is ``NULL``, no such retention is offered and the callee must
copy anything it needs before returning.

Callbacks run on the PMIx progress thread. They should therefore complete
quickly and must not block waiting on other PMIx operations. The ``cbdata``
object and any other input parameters supplied to the originating call must
remain valid until the callback fires.


CALLBACK FUNCTIONS
------------------

.. _man5-pmix_release_cbfunc_t:

pmix_release_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_release_cbfunc_t)(void *cbdata);

The fundamental "done with the data" callback. It carries no status and only
the opaque ``cbdata`` that was provided alongside it. Data-returning callbacks
that transfer ownership temporarily (for example
:ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>`,
:ref:`pmix_modex_cbfunc_t <man5-pmix_modex_cbfunc_t>`, and
:ref:`pmix_device_dist_cbfunc_t <man5-pmix_device_dist_cbfunc_t>`) also pass a
``release_fn`` of this type together with a ``release_cbdata``; the recipient
calls it once it has finished using the retained data so the library can
release it.


.. _man5-pmix_op_cbfunc_t:

pmix_op_cbfunc_t
^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The generic callback for operations that simply return a completion status.
``status`` reports success or failure and ``cbdata`` is the caller's opaque
pointer. This is the most widely used callback in PMIx; examples include the
non-blocking forms of Fence, Connect, Disconnect, Put/Commit, and event
notification, as well as many server-side APIs.


.. _man5-pmix_info_cbfunc_t:

pmix_info_cbfunc_t
^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_info_cbfunc_t)(pmix_status_t status,
                                      pmix_info_t *info, size_t ninfo,
                                      void *cbdata,
                                      pmix_release_cbfunc_t release_fn,
                                      void *release_cbdata);

Returns an array of ``ninfo`` :ref:`pmix_info_t(5) <man5-pmix_info_t>`
key/value pairs in ``info``. Used to deliver the results of queries such as
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`. The ``info`` array is owned
by the library; if ``release_fn`` is non-``NULL`` the recipient may retain the
array and must call ``release_fn(release_cbdata)`` when done, otherwise it must
copy any values it needs before returning.


.. _man5-pmix_modex_cbfunc_t:

pmix_modex_cbfunc_t
^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_modex_cbfunc_t)(pmix_status_t status,
                                       const char *data, size_t ndata,
                                       void *cbdata,
                                       pmix_release_cbfunc_t release_fn,
                                       void *release_cbdata);

A server-side callback used to return modex ("modular exchange") data
collected in response to fence and remote "get" operations. ``data`` points to
an opaque blob of ``ndata`` bytes aggregated from the participating servers.
The blob is owned by the host server, so a non-``NULL`` ``release_fn`` is
provided to notify the owner via ``release_fn(release_cbdata)`` once the
recipient is finished with the data.


.. _man5-pmix_value_cbfunc_t:

pmix_value_cbfunc_t
^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_value_cbfunc_t)(pmix_status_t status,
                                       pmix_value_t *kv, void *cbdata);

Returns a single retrieved value. Used by :ref:`PMIx_Get(3) <man3-PMIx_Get>`
in its non-blocking form. ``status`` indicates whether the requested key was
found; ``kv`` points to the :ref:`pmix_value_t(5) <man5-pmix_value_t>`
containing the data, or is ``NULL`` if the data was not found. The value is
released by the library on return, so the recipient must copy it if it needs to
be retained.


.. _man5-pmix_lookup_cbfunc_t:

pmix_lookup_cbfunc_t
^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_lookup_cbfunc_t)(pmix_status_t status,
                                        pmix_pdata_t data[], size_t ndata,
                                        void *cbdata);

Returns the results of a non-blocking data lookup such as
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>` (the companion of
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>`). Retrieved key/value pairs are
returned as an array of ``ndata`` :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
structures, each also identifying the namespace/rank of the process that
published the item. The structures are released on return from the callback, so
the recipient must copy anything it needs to retain.


.. _man5-pmix_spawn_cbfunc_t:

pmix_spawn_cbfunc_t
^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_spawn_cbfunc_t)(pmix_status_t status,
                                       pmix_nspace_t nspace, void *cbdata);

Reports completion of a non-blocking :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`
request. ``status`` indicates whether the spawn succeeded, and ``nspace``
carries the namespace assigned to the newly spawned job. The returned
``nspace`` value is released by the library upon return from the callback, so
the recipient must copy it if it needs to be retained.


.. _man5-pmix_credential_cbfunc_t:

pmix_credential_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_credential_cbfunc_t)(pmix_status_t status,
                                            pmix_byte_object_t *credential,
                                            pmix_info_t info[], size_t ninfo,
                                            void *cbdata);

Returns a requested security credential obtained via
:ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>`. On success,
``credential`` points to an allocated ``pmix_byte_object_t`` holding the opaque
credential blob; ownership of the credential is transferred to the recipient,
which is responsible for releasing that memory. The ``info`` array (``ninfo``
elements) conveys any additional system-provided metadata about the credential,
such as the identity of the issuing agent; that array is owned by the library
and must not be altered or released by the recipient.


.. _man5-pmix_validation_cbfunc_t:

pmix_validation_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_validation_cbfunc_t)(pmix_status_t status,
                                            pmix_info_t info[], size_t ninfo,
                                            void *cbdata);

Reports the result of validating a credential via
:ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>`. ``status``
is ``PMIX_SUCCESS`` if the credential is valid, or an error code describing why
it was rejected. The ``info`` array (``ninfo`` elements) carries any associated
authorization information |mdash| commonly including the effective
``PMIX_USERID`` and ``PMIX_GROUPID`` of the credential's holder. The array is
owned by the library and must not be altered or released by the recipient.


.. _man5-pmix_device_dist_cbfunc_t:

pmix_device_dist_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_device_dist_cbfunc_t)(pmix_status_t status,
                                             pmix_device_distance_t *dist,
                                             size_t ndist,
                                             void *cbdata,
                                             pmix_release_cbfunc_t release_fn,
                                             void *release_cbdata);

Returns computed device distance arrays from
:ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`. ``dist`` points
to an array of ``ndist`` ``pmix_device_distance_t`` structures describing the
relative locality of the calling process to each fabric or other device. The
array is owned by the library; if ``release_fn`` is non-``NULL`` the recipient
may retain it and must call ``release_fn(release_cbdata)`` when finished,
otherwise it must copy the data before returning.


.. _man5-pmix_hdlr_reg_cbfunc_t:

pmix_hdlr_reg_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_hdlr_reg_cbfunc_t)(pmix_status_t status,
                                          size_t refid,
                                          void *cbdata);

Reports completion of a handler registration request, such as registering an
event handler or an IOF handler. ``status`` is ``PMIX_SUCCESS`` or an error
constant, and ``refid`` is the reference identifier assigned to the handler by
PMIx |mdash| it must be retained in order to later deregister the handler.


.. _man5-pmix_evhdlr_reg_cbfunc_t:

pmix_evhdlr_reg_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_evhdlr_reg_cbfunc_t)(pmix_status_t status,
                                            size_t refid,
                                            void *cbdata);

The deprecated, event-handler-specific form of
:ref:`pmix_hdlr_reg_cbfunc_t <man5-pmix_hdlr_reg_cbfunc_t>`, retained for
backward compatibility. It has an identical signature and reports the
registration ``status`` and assigned reference identifier ``refid`` for a call
to :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`.
New code should use ``pmix_hdlr_reg_cbfunc_t``.


.. _man5-pmix_notification_fn_t:

pmix_notification_fn_t
^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_notification_fn_t)(size_t evhdlr_registration_id,
                                          pmix_status_t status,
                                          const pmix_proc_t *source,
                                          pmix_info_t info[], size_t ninfo,
                                          pmix_info_t *results, size_t nresults,
                                          pmix_event_notification_cbfunc_fn_t cbfunc,
                                          void *cbdata);

The user-supplied event handler. PMIx invokes it when an event is delivered to
a handler registered via
:ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`
(events are generated with
:ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`). Arguments are the
handler's ``evhdlr_registration_id``; the ``status`` event code that occurred;
the ``source`` process that generated it (an empty namespace with rank
``PMIX_RANK_UNDEF`` denotes the resource manager); an ``info`` array of
``ninfo`` event details; a ``results`` array of ``nresults`` outputs from any
handlers already run for this event; and a completion ``cbfunc`` (of type
:ref:`pmix_event_notification_cbfunc_fn_t <man5-pmix_event_notification_cbfunc_fn_t>`)
with its ``cbdata``. The handler is **required** to invoke ``cbfunc`` when done
so the library can continue the event chain. The ``info`` and ``results``
arrays are owned by the library and are valid only for the duration of the
call.


.. _man5-pmix_event_notification_cbfunc_fn_t:

pmix_event_notification_cbfunc_fn_t
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_event_notification_cbfunc_fn_t)(pmix_status_t status,
                                                       pmix_info_t *results, size_t nresults,
                                                       pmix_op_cbfunc_t cbfunc, void *thiscbdata,
                                                       void *notification_cbdata);

The completion callback that an event handler
(:ref:`pmix_notification_fn_t <man5-pmix_notification_fn_t>`) must call to tell
the library it has finished responding to a notification. The handler passes
back a ``status`` |mdash| ``PMIX_SUCCESS`` if no further action is required
|mdash| along with an optional ``results`` array of ``nresults``
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structures that are appended to the
results seen by subsequent handlers in the chain. ``cbfunc``/``thiscbdata``
allow the library to signal the handler once it has consumed the results (so a
non-``NULL`` ``cbfunc`` lets the handler release its ``results`` array), and
``notification_cbdata`` is the library's internal chaining context, passed back
unmodified.


.. _man5-pmix_setup_application_cbfunc_t:

pmix_setup_application_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_setup_application_cbfunc_t)(pmix_status_t status,
                                                   pmix_info_t info[], size_t ninfo,
                                                   void *provided_cbdata,
                                                   pmix_op_cbfunc_t cbfunc, void *cbdata);

A server-side callback used to return the results of
:ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`.
The ``info`` array (``ninfo`` elements) contains application-specific
environment variables, resource assignments, and other data (for example
network security credentials) to be distributed with the application at launch.
``provided_cbdata`` is the opaque pointer supplied to the setup call. The
``info`` array is owned by the PMIx server library and remains valid until the
recipient invokes the provided ``cbfunc`` (a
:ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>`) with ``cbdata``, which
releases it.


.. _man5-pmix_connection_cbfunc_t:

pmix_connection_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_connection_cbfunc_t)(int incoming_sd, void *cbdata);

Used by a host server that operates its own connection-listener thread to hand
an accepted local-client connection to the PMIx server library. After calling
``accept`` on an incoming connection request, the host passes the resulting
socket descriptor ``incoming_sd`` to this callback for further processing,
along with its opaque ``cbdata``.


.. _man5-pmix_tool_connection_cbfunc_t:

pmix_tool_connection_cbfunc_t
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_tool_connection_cbfunc_t)(pmix_status_t status,
                                                 pmix_proc_t *proc, void *cbdata);

Used when a host environment registers a newly connected tool with the PMIx
server library and must assign it an identity. The host returns the assigned
namespace/rank in ``proc`` (rank 0 is the normal assignment), with ``status``
indicating success or failure and ``cbdata`` the opaque pointer. This is the
server-side counterpart to a tool's use of
:ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`.


.. _man5-pmix_dmodex_response_fn_t:

pmix_dmodex_response_fn_t
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_dmodex_response_fn_t)(pmix_status_t status,
                                             char *data, size_t sz,
                                             void *cbdata);

Returns the data blob produced by a direct-modex request made through
:ref:`PMIx_server_dmodex_request(3) <man3-PMIx_server_dmodex_request>`.
``data`` points to a blob of ``sz`` bytes that the host server should send back
to the original remote requestor; ``status`` reports success or failure and
``cbdata`` is the opaque pointer. The PMIx server frees the ``data`` blob upon
return from this response function, so the recipient must copy or transmit it
before returning.


.. _man5-pmix_iof_cbfunc_t:

pmix_iof_cbfunc_t
^^^^^^^^^^^^^^^^^

.. code-block:: c

   typedef void (*pmix_iof_cbfunc_t)(size_t iofhdlr, pmix_iof_channel_t channel,
                                     pmix_proc_t *source, pmix_byte_object_t *payload,
                                     pmix_info_t info[], size_t ninfo);

Delivers forwarded I/O (stdout/stderr) to a handler registered via
:ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`. ``iofhdlr`` is the registration
number of the handler being invoked (needed to deregister it); ``channel`` is a
bitmask identifying the I/O channel the data arrived on; ``source`` is the
namespace/rank that generated the output; and ``payload`` points to a
``pmix_byte_object_t`` containing the data (which may hold multiple strings and
is **not** guaranteed to be NUL-terminated). The optional ``info`` array
(``ninfo`` elements) carries metadata about the payload, such as
``PMIX_IOF_COMPLETE``. Note this callback receives no ``cbdata`` and no
``status`` argument.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>`,
   :ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   :ref:`PMIx_server_dmodex_request(3) <man3-PMIx_server_dmodex_request>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
