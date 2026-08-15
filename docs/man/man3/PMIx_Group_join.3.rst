.. _man3-PMIx_Group_join:

PMIx_Group_join
===============

.. include_body

``PMIx_Group_join``, ``PMIx_Group_join_nb`` |mdash| Respond to an invitation to
join an asynchronously-constructed PMIx group.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Group_join(const char grp[],
                                 const pmix_proc_t *leader,
                                 pmix_group_opt_t opt,
                                 const pmix_info_t info[], size_t ninfo,
                                 pmix_info_t **results, size_t *nresult);

   pmix_status_t PMIx_Group_join_nb(const char grp[],
                                    const pmix_proc_t *leader,
                                    pmix_group_opt_t opt,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the leader is a Python ``pmix_proc_t`` dictionary
  leader = {'nspace': "testnspace", 'rank': 0}
  # opt is PMIX_GROUP_ACCEPT or PMIX_GROUP_DECLINE
  # the info is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = []
  rc, results = foo.group_join("mygroup", leader, PMIX_GROUP_ACCEPT, pyinfo)

An invitation is delivered in an event handler, and the blocking form
must not be called from one. The non-blocking form is therefore the
usual way to respond: it takes a Python callback in place of the
returned results, and reports only the status of the request itself:

.. code-block:: python3

  from pmix import *

  # the callback is executed on the PMIx progress thread once the group
  # has formed. It must not make a blocking PMIx call
  def mycb(status:int, results:list, cbdata):
      print("JOIN COMPLETE", status, results)

  def myinvitehdlr(evhdlr:int, status:int,
                   source:dict, info:list, results:list):
      # accepting from within the handler is safe in this form
      foo.group_join_nb("mygroup", source, PMIX_GROUP_ACCEPT, [], mycb, None)
      return PMIX_EVENT_ACTION_COMPLETE, None


INPUT PARAMETERS
----------------

* ``grp``: A NULL-terminated character string identifying the group named in the
  invitation. The string must be of length less than or equal to
  ``PMIX_MAX_NSLEN``.
* ``leader``: Pointer to a ``pmix_proc_t`` structure identifying the process that
  issued the invitation (the leader of the asynchronous construction procedure).
* ``opt``: A ``pmix_group_opt_t`` value indicating the response |mdash|
  ``PMIX_GROUP_ACCEPT`` to join the group or ``PMIX_GROUP_DECLINE`` to reject the
  invitation.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs
  conveying user directives that qualify the operation.
* ``ninfo``: Number of elements in the ``info`` array.

The blocking form returns results directly:

* ``results``: Address of a pointer that, upon successful return, is set to an
  array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs containing any values
  returned by the operation. May be ``NULL`` if the caller does not want the
  returned values. The caller is responsible for releasing the array with
  ``PMIX_INFO_FREE``.
* ``nresult``: Address of a ``size_t`` that is set to the number of elements in
  the returned ``results`` array.

The non-blocking form takes two additional parameters in place of ``results``
and ``nresult``:

* ``cbfunc``: Callback function of type :ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>` to be executed once
  the group has been completely constructed or its construction has failed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Respond to an invitation to join a group that is being asynchronously constructed
via :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`. The process must have
registered for the ``PMIX_GROUP_INVITED`` event in order to have been notified of
the invitation.

Calling this function causes the group leader to be notified that the process has
either accepted or declined the request (the ``opt`` argument) |mdash| via a
``PMIX_GROUP_INVITE_ACCEPTED`` or ``PMIX_GROUP_INVITE_DECLINED`` event,
respectively. The blocking form returns once the group has been completely
constructed or its construction has failed (as determined by the leader); likewise,
the callback function of the non-blocking form is executed upon the same
conditions.

An acceptance also carries this process's own endpoint data to the leader
|mdash| exactly the values it posted with :ref:`PMIx_Put(3) <man3-PMIx_Put>` at
``PMIX_REMOTE`` or ``PMIX_GLOBAL`` scope, less any reserved key. The leader
assembles the contributions of all the accepting members and returns them in
the ``PMIX_GROUP_CONSTRUCT_COMPLETE`` event, where each member absorbs them
into its local store; a subsequent :ref:`PMIx_Get(3) <man3-PMIx_Get>` against
another member is then satisfied without leaving the process. This is the same
exchange :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>` performs
through its collective, and the same rule applies: what is exchanged is what
the members put, so anything the runtime computed rather than the application
posting it is job-level data and does not travel this way. A declining process
joins nothing and contributes nothing.

An accepting process learns of that outcome from the leader's
``PMIX_GROUP_CONSTRUCT_COMPLETE`` or ``PMIX_GROUP_CONSTRUCT_ABORT`` event, so it is
those events that complete this call, and ``results`` carries what the leader
announced about the group |mdash| its ``PMIX_GROUP_ID``, its
``PMIX_GROUP_MEMBERSHIP``, and its ``PMIX_GROUP_CONTEXT_ID`` if the leader
requested one and the host was able to assign it. The contributed endpoint data
is *not* reported in ``results``: it is not something the join produced, and it
is stored for retrieval through :ref:`PMIx_Get(3) <man3-PMIx_Get>` instead.
Loss of the leader before the construct resolves completes the call with
``PMIX_GROUP_LEADER_FAILED``, since the construct can no longer resolve.

Two cases have no construct outcome to wait for, and so complete as soon as the
notification to the leader has been issued, with ``results`` empty: declining the
invitation (``PMIX_GROUP_DECLINE``), and accepting without naming a ``leader``.

.. note::

   Prior to the introduction of the ``PMIX_CAP_GROUP_JOIN_COMPLETES`` capability
   flag, this call completed as soon as the accept/decline notification had been
   handed to the local event system |mdash| much earlier than described above, and
   ``results`` was always returned empty. Applications written against that
   behavior will now block for the duration of the construct. Consult
   ``pmix_version.h`` for the presence of the flag.

As with all non-blocking PMIx APIs, callers of ``PMIx_Group_join_nb``
**must** keep the ``grp``, ``leader``, and ``info`` arrays valid until ``cbfunc`` is
invoked. The invite/join handshake is described in full in
:ref:`Group Construction, Destruction, and Fault Tolerance <group-construction-label>`.

.. caution::

   Because the process is alerted to the invitation in a PMIx event handler, it
   **must not** use the blocking form of this call unless it first "thread shifts"
   out of the handler and into its own thread context. Likewise, while it is safe
   to call the non-blocking form from the event handler, the process must not block
   in the handler while waiting for the callback function to be invoked.

Leadership
^^^^^^^^^^

Failure of the leader at any time causes a ``PMIX_GROUP_LEADER_FAILED`` event to be
delivered to all participants so they can optionally declare a new leader. A new
leader is identified by providing the ``PMIX_GROUP_LEADER`` attribute in the results
array returned from the event handler; only one process is permitted to do so. The
outcome of leader selection is communicated to all participants via a
``PMIX_GROUP_LEADER_SELECTED`` event. If no leader was selected, the status code in
that event carries an error value so participants can take appropriate action.

Any participant that returns ``PMIX_GROUP_CONSTRUCT_ABORT`` from the leader-failed
event handler causes all participants to receive an event notifying them of that
status. Similarly, the leader may elect to abort the procedure |mdash| either by
returning ``PMIX_GROUP_CONSTRUCT_ABORT`` from the handler assigned to the
``PMIX_GROUP_INVITE_ACCEPTED`` or ``PMIX_GROUP_INVITE_DECLINED`` codes, or by
generating an event for the abort code. Abort events are sent to all invited
participants.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the group was successfully
constructed and any returned values are available in ``results``. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request was
successfully accepted for processing; the final status is delivered to ``cbfunc``,
and the callback is not invoked if any other value is returned.

* ``PMIX_SUCCESS`` |mdash| the group was successfully constructed.
* ``PMIX_GROUP_CONSTRUCT_ABORT`` |mdash| construction of the group was aborted.
* ``PMIX_GROUP_LEADER_FAILED`` |mdash| the leader was lost before the construct
  resolved, so it never will.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a required argument was invalid (e.g., a ``NULL``
  or over-length group identifier).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support group
  operations.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other value indicates an appropriate error condition.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`,
   :ref:`PMIx_Group_leave(3) <man3-PMIx_Group_leave>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_group_opt_t(5) <man5-pmix_group_opt_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
