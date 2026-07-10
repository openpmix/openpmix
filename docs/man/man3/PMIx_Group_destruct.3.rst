.. _man3-PMIx_Group_destruct:

PMIx_Group_destruct
===================

.. include_body

``PMIx_Group_destruct``, ``PMIx_Group_destruct_nb`` |mdash| Collectively destruct
a PMIx group.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Group_destruct(const char grp[],
                                     const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Group_destruct_nb(const char grp[],
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the info is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = []
  rc = foo.group_destruct("mygroup", pyinfo)


INPUT PARAMETERS
----------------

* ``grp``: A NULL-terminated character string identifying the group to be
  destructed. The string must be of length less than or equal to
  ``PMIX_MAX_NSLEN``.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs
  conveying user directives that qualify the operation (see `DIRECTIVES`_).
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` to be executed once all
  members of the group have called destruct.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Destruct a group identified by the provided group identifier. Destruct is a
collective over the *current* membership: the blocking form does not return, and the
non-blocking form does not invoke its callback, until all members of the group have
called destruct (or a fault-handling rule resolves the operation). This is the
recommended, scalable way to tear down a group and should be preferred over
:ref:`PMIx_Group_leave(3) <man3-PMIx_Group_leave>` in all cases except the
asynchronous departure of an individual process.

Because the group identifier and its membership are held by the client libraries
|mdash| not the host |mdash| the destruct call carries the membership to its local
server so the server knows how many local participants to await. On completion, the
group identifier and any assigned context ID are released, and the group is removed
from each member's local group list.

Processes may engage in multiple simultaneous group destruct operations, so long as
each involves a unique group ID. As with all non-blocking PMIx APIs, callers of
``PMIx_Group_destruct_nb`` **must** keep the ``grp`` and ``info`` arrays valid until
``cbfunc`` is invoked. These behaviors are described in full in
:ref:`Group Construction, Destruction, and Fault Tolerance <group-construction-label>`.

Member failure
^^^^^^^^^^^^^^

The destruct operation completes on the surviving members if any group process
fails or terminates prior to calling ``PMIx_Group_destruct`` (or its non-blocking
form) |mdash| it does not hang waiting on the lost member. How that completion is
reported depends on ``PMIX_GROUP_NOTIFY_TERMINATION``:

* **Default (no notification).** The teardown is reported as an error: the degraded
  status (``PMIX_ERR_LOST_CONNECTION``) is recorded for the host, and no event is
  generated.
* **With ``PMIX_GROUP_NOTIFY_TERMINATION``.** A ``PMIX_GROUP_MEMBER_FAILED`` event
  naming the departed process is delivered to each surviving member, the destruct
  tracker is updated to account for the lack of participation, and
  ``PMIx_Group_destruct`` returns ``PMIX_SUCCESS`` once the remaining processes have
  all called destruct |mdash| i.e., the event serves in place of the return of an
  error.

``PMIX_GROUP_NOTIFY_TERMINATION`` is a property of the group's failure policy and is
supplied at *construct* time (see
:ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`). The library remembers
it and re-applies it to this destruct on the caller's behalf, so it need not be
passed to ``PMIx_Group_destruct`` again; passing it directly here is also honored
and overrides the remembered policy.


DIRECTIVES
----------

The following attributes are relevant to this operation:

* ``PMIX_GROUP_NOTIFY_TERMINATION`` (bool) |mdash| report, rather than error, a
  member lost before it calls destruct (see `Member failure`_). Supplied at
  construct time and re-applied here automatically; may also be passed directly to
  this call.
* ``PMIX_TIMEOUT`` (int) |mdash| return an error if the group does not destruct
  within the specified number of seconds. This targets the scenario where a process
  fails to call ``PMIx_Group_destruct`` due to hanging.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the group was successfully
destructed. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only
that the request was successfully accepted for processing; the final status is
delivered to ``cbfunc``, and the callback is not invoked if any other value is
returned.

* ``PMIX_SUCCESS`` |mdash| the group was successfully destructed. Also returned on
  the loss of a member when ``PMIX_GROUP_NOTIFY_TERMINATION`` was in effect, with the
  loss reported via a ``PMIX_GROUP_MEMBER_FAILED`` event instead (see
  `Member failure`_).
* ``PMIX_ERR_LOST_CONNECTION`` |mdash| a member was lost before calling destruct and
  ``PMIX_GROUP_NOTIFY_TERMINATION`` was not in effect; the destruct still completed
  on the surviving members. (The exact status a caller observes for this degraded
  case is determined by the host environment.)
* ``PMIX_ERR_BAD_PARAM`` |mdash| a required argument was invalid (e.g., a ``NULL``
  or over-length group identifier).
* ``PMIX_ERR_TIMEOUT`` |mdash| the group did not destruct within the time specified
  by ``PMIX_TIMEOUT``.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support group
  operations.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other value indicates an appropriate error condition, such as the failure of a
member process when ``PMIX_GROUP_NOTIFY_TERMINATION`` was not requested.


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`,
   :ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>`,
   :ref:`PMIx_Group_leave(3) <man3-PMIx_Group_leave>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
