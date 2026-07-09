.. _man3-PMIx_Group_leave:

PMIx_Group_leave
================

.. include_body

``PMIx_Group_leave``, ``PMIx_Group_leave_nb`` |mdash| Asynchronously depart from a
PMIx group.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Group_leave(const char grp[],
                                  const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Group_leave_nb(const char grp[],
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the info is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = []
  rc = foo.group_leave("mygroup", pyinfo)


INPUT PARAMETERS
----------------

* ``grp``: A NULL-terminated character string identifying the group the caller
  wishes to leave. The string must be of length less than or equal to
  ``PMIX_MAX_NSLEN``.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs
  conveying user directives that qualify the operation.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be executed once the
  departure event has been locally generated.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Leave a PMIx group. A call to ``PMIx_Group_leave`` (or its non-blocking form)
causes a ``PMIX_GROUP_LEFT`` event to be generated, ranged to the group's current
membership and naming the departing process. The function returns (or the
non-blocking form executes the specified callback) once the event has been locally
generated; the return is *not* indicative of remote receipt. The departing process
drops the group from its own local list immediately, and each remaining member, on
receiving ``PMIX_GROUP_LEFT``, updates its local record of the group membership to
remove the departed process.

A leave is realized entirely through the event subsystem. A voluntary leave is
treated by the library as the deliberate cousin of a lost member: if a group
construct or destruct for that same group is still in flight when the leave
arrives, the leaving process is accounted as "departed" so the in-flight collective
can complete on the survivors rather than block on a process that will never call
in. More generally, all PMIx-based collectives (such as ``PMIx_Fence``) in action
across the group are automatically adjusted if the collective was invoked with the
``PMIX_GROUP_FT_COLLECTIVE`` attribute (default is ``false``); otherwise, the
standard error-return behavior is provided.

As with all non-blocking PMIx APIs, callers of ``PMIx_Group_leave_nb`` **must** keep
the ``grp`` and ``info`` arrays valid until ``cbfunc`` is invoked.

.. note::

   The ``PMIx_Group_leave`` API is intended solely for the asynchronous departure
   of individual processes from a group, and is not a scalable operation |mdash|
   i.e., for the case where a single process determines it should no longer be part
   of a group while the remainder of the group retains a valid reason to continue
   in existence. For all other scenarios |mdash| especially coordinated teardown
   |mdash| developers are advised to use
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>` (or its non-blocking
   form), which represents a more scalable operation.

This behavior is described in full in
:ref:`Group Construction, Destruction, and Fault Tolerance <group-construction-label>`.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the departure event was
successfully generated locally. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was successfully accepted for
processing; the final status is delivered to ``cbfunc``, and the callback is not
invoked if any other value is returned.

* ``PMIX_SUCCESS`` |mdash| the departure was successfully processed.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a required argument was invalid (e.g., a ``NULL``
  or over-length group identifier).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support group
  operations.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other value indicates an appropriate error condition.


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`,
   :ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
