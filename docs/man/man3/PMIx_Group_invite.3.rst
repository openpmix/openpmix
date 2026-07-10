.. _man3-PMIx_Group_invite:

PMIx_Group_invite
=================

.. include_body

``PMIx_Group_invite``, ``PMIx_Group_invite_nb`` |mdash| Invite specified
processes to asynchronously join a PMIx group.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Group_invite(const char grp[],
                                   const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t info[], size_t ninfo,
                                   pmix_info_t **results, size_t *nresult);

   pmix_status_t PMIx_Group_invite_nb(const char grp[],
                                      const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t info[], size_t ninfo,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the peers is a list of Python ``pmix_proc_t`` dictionaries
  peers = [{'nspace': "testnspace", 'rank': 1},
           {'nspace': "testnspace", 'rank': 2}]
  # the info is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = []
  rc, results = foo.group_invite("mygroup", peers, pyinfo)


INPUT PARAMETERS
----------------

* ``grp``: A NULL-terminated character string identifying the group. The string
  must be of length less than or equal to ``PMIX_MAX_NSLEN`` and may contain only
  characters accepted by the standard string comparison functions (e.g.,
  ``strncmp``).
* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming the
  processes being invited to join the group.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs
  conveying user directives that qualify the operation (see `DIRECTIVES`_).
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
  the group has been fully assembled.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Explicitly invite the specified processes to join a group. This is the most
dynamic form of group construction, in contrast to the collective
:ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>` in which all members
call the API to assemble the group. The invite method involves no collective
operation: it is executed in an ad hoc manner around a single leader (the inviting
process) and relies solely on the event notification subsystem for its underlying
execution.

Each invited process is notified of the invitation via the ``PMIX_GROUP_INVITED``
event. Invited processes **must** have registered for that event in order to be
notified; the handler retrieves the ``PMIX_GROUP_ID`` from the event and, when
ready to respond, replies using the non-blocking
:ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>` (the blocking form cannot be used
from within the handler). This notifies the inviting process that the invitation
was either accepted (via the ``PMIX_GROUP_INVITE_ACCEPTED`` event) or declined (via
the ``PMIX_GROUP_INVITE_DECLINED`` event). To receive the final membership, each
participant should also register for the ``PMIX_GROUP_CONSTRUCT_COMPLETE`` event,
which is delivered once the group has assembled.

Upon acceptance of the invitation, both the inviting and the invited process gain
access to the job-level information of each other's namespaces and the contact
information of the other process.

The inviting process may register for the ``PMIX_GROUP_INVITE_FAILED`` event to be
notified whenever an invited process declines, terminates before responding, or
fails to respond within a supplied ``PMIX_TIMEOUT``. In response, the leader can
replace the failed invitee with another, abort the construct, or (with
``PMIX_GROUP_OPTIONAL``) allow the group to form with reduced membership |mdash|
see `DIRECTIVES`_.

This method, and the responsibilities of the client library, server, and host, are
described in full in
:ref:`Group Construction, Destruction, and Fault Tolerance <group-construction-label>`.

``PMIx_Group_invite`` is the blocking form; ``PMIx_Group_invite_nb`` is the
non-blocking form, returning immediately and delivering the final status and any
returned values through ``cbfunc``. As with all non-blocking PMIx APIs, callers of
``PMIx_Group_invite_nb`` **must** keep the ``grp``, ``procs``, and ``info`` arrays
valid until ``cbfunc`` is invoked.

Leadership
^^^^^^^^^^

The inviting process is automatically considered the leader of the asynchronous
group construction procedure and receives all failure or termination events for
invited members prior to completion. Once the group has been fully assembled, the
client library assembles the completion payload and distributes a
``PMIX_GROUP_CONSTRUCT_COMPLETE`` event to all participants along with the final
membership; the leader returns from ``PMIx_Group_invite`` (or has its callback
invoked) at that point.

Failure of the leader at any time causes a ``PMIX_GROUP_LEADER_FAILED`` event to be
delivered to all participants so they can optionally declare a new leader. A new
leader is identified by providing the ``PMIX_GROUP_LEADER`` attribute in the
results array returned from the event handler; only one process is permitted to do
so. The outcome of leader selection is communicated to all participants via a
``PMIX_GROUP_LEADER_SELECTED`` event. If no leader was selected, the status code in
that event carries an error value so participants can take appropriate action.

Any participant that returns ``PMIX_GROUP_CONSTRUCT_ABORT`` from the event handler
causes all participants to receive an event notifying them of that status.


DIRECTIVES
----------

The following attributes are relevant to this operation:

* ``PMIX_GROUP_OPTIONAL`` (bool) |mdash| govern the outcome when an invitee fails
  to accept (declines, terminates, or times out). By default (``false``) the
  construct is all-or-nothing: any such failure aborts the whole operation, every
  participant receives ``PMIX_GROUP_CONSTRUCT_ABORT``, and no group forms. When set
  to ``true``, the failed invitees are simply excluded and the group forms on those
  that accepted, each receiving ``PMIX_GROUP_CONSTRUCT_COMPLETE`` with the reduced
  membership.
* ``PMIX_GROUP_ASSIGN_CONTEXT_ID`` (bool) |mdash| request that the resource
  manager assign a unique numerical (``size_t``) context ID to the group. The
  value is returned in the ``PMIX_GROUP_CONSTRUCT_COMPLETE`` event.
* ``PMIX_TIMEOUT`` (int) |mdash| bound how long the leader's library waits for a
  live invitee to respond. An invitee that does not respond within the specified
  number of seconds is treated as a failed invitation (see
  ``PMIX_GROUP_OPTIONAL``).


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the group was successfully
assembled and any returned values are available in ``results``. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request
was successfully accepted for processing; the final status is delivered to
``cbfunc``, and the callback is not invoked if any other value is returned.

* ``PMIX_SUCCESS`` |mdash| the group was successfully assembled.
* ``PMIX_GROUP_CONSTRUCT_ABORT`` |mdash| a participant aborted the construction
  procedure.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a required argument was invalid (e.g., a ``NULL``
  or over-length group identifier).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support group
  operations.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other value indicates an appropriate error condition.


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>`,
   :ref:`PMIx_Group_leave(3) <man3-PMIx_Group_leave>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
