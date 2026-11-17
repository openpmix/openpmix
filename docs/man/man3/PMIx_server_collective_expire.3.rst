.. _man3-PMIx_server_collective_expire:

PMIx_server_collective_expire
=============================

.. include_body

``PMIx_server_collective_expire`` |mdash| Hand a collective that is still
collecting its local participants to the host now, as though it had timed out.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_collective_expire(pmix_collective_op_t op, const char *id,
                                               const pmix_proc_t procs[], size_t nprocs,
                                               const pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # a group construct, named by its ID
  rc = foo.collective_expire(PMIX_COLLECTIVE_GROUP_CONSTRUCT, "mygroup", None, [])
  # a fence, named by its procs
  rc = foo.collective_expire(PMIX_COLLECTIVE_FENCE, None, procs, [])
  # either, non-blocking
  rc = foo.collective_expire(PMIX_COLLECTIVE_FENCE, None, procs, [], cbfunc, cbdata)


INPUT PARAMETERS
----------------

* ``op``: The collective, a ``pmix_collective_op_t``: ``PMIX_COLLECTIVE_FENCE``,
  ``PMIX_COLLECTIVE_CONNECT``, ``PMIX_COLLECTIVE_DISCONNECT``,
  ``PMIX_COLLECTIVE_GROUP_CONSTRUCT`` or ``PMIX_COLLECTIVE_GROUP_DESTRUCT``.
* ``id``: The group ID, for a group construct or destruct. Ignored for the others.
* ``procs``: The processes the collective names, as the server gave them to the
  host, for a fence, connect or disconnect. Ignored for a group.
* ``nprocs``: Number of elements in ``procs``.
* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` directives. None are
  defined yet.
* ``ninfo``: Number of elements in ``info``.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>`
  invoked when the operation completes. A ``NULL`` value makes the call
  **blocking**.
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

A PMIx server collects a collective's local participants and hands it to the
host once every one of them has called. If a participant gave a
``PMIX_TIMEOUT``, the server's own timer bounds that wait, and when it fires the
server hands the collective up as it stands: ``PMIX_ERR_TIMEOUT`` as the
``PMIX_LOCAL_COLLECTIVE_STATUS``, and ``PMIX_LOCAL_PARTICIPANTS`` naming the
local processes that had called. Each server's timer starts when its own first
local participant calls, so the servers' deadlines differ.

``PMIx_server_collective_expire`` lets the host impose one deadline on all of
them. Called on a server that is still collecting the named collective, it hands
the collective up at once, exactly as the server's timer would have: the host's
``fence_nb``, ``connect``, ``disconnect`` or ``group`` entry point is called with
the timeout status and the participants so far. A host that expires the
collective at every server learns, from every server, who had taken part by the
moment it chose, and can name the members that had not.

The host completes an expired collective like any other, with the status it
decides on. A process that calls the collective after it has been expired at its
server is answered ``PMIX_ERR_TIMEOUT``: it missed the deadline.

A server holding no such collective still collecting |mdash| none of its local
participants has called, the collective is already with the host, or it has no
host to hand it to |mdash| reports ``PMIX_ERR_NOT_FOUND`` and does nothing. A
follower's or bootstrap participant's group call goes to the host as soon as it
is made, so there is nothing of it to expire.

See :ref:`collective-participants-label` for how a host uses the participant
lists, and how it returns the members that never took part.

``PMIx_server_collective_expire`` supports both calling conventions. When
``cbfunc`` is non-``NULL`` the function is **non-blocking**: the request is
thread-shifted onto the progress thread, the function returns ``PMIX_SUCCESS``,
and ``cbfunc`` is invoked with the result. When ``cbfunc`` is ``NULL`` the
function is **blocking**; called from the progress thread, as from within a host
upcall, it returns ``PMIX_ERR_WOULD_BLOCK`` rather than wait for itself.

As with all non-blocking PMIx APIs, when a callback is supplied the caller
**must** keep ``id``, ``procs`` and ``info`` valid until ``cbfunc`` is invoked.


RETURN VALUE
------------

For the non-blocking form, ``PMIX_SUCCESS`` means the request was accepted, and
``cbfunc`` then reports ``PMIX_SUCCESS`` if the collective was handed to the
host, or ``PMIX_ERR_NOT_FOUND`` if there was none to hand up.

The blocking form returns ``PMIX_OPERATION_SUCCEEDED`` if the collective was
handed to the host, or ``PMIX_ERR_NOT_FOUND`` if there was none. Other returns
include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the library's progress engine has been
  stopped.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an unknown ``op``, a group operation with no
  ``id``, or a fence, connect or disconnect with no ``procs``.
* ``PMIX_ERR_WOULD_BLOCK`` |mdash| the blocking form was called from the
  progress thread.
