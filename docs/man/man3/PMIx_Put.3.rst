.. _man3-PMIx_Put:

PMIx_Put
========

.. include_body

``PMIx_Put`` |mdash| Stage a key/value pair for distribution to other processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Put(pmix_scope_t scope,
                          const char key[],
                          pmix_value_t *val);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the value is a Python ``pmix_value_t`` dictionary
  rc = foo.put(PMIX_GLOBAL, "mykey",
               {'value': 42, 'val_type': PMIX_UINT32})


INPUT PARAMETERS
----------------

* ``scope``: A ``pmix_scope_t`` value describing the distribution scope of the
  posted data |mdash| i.e., which processes are to be able to access it (see
  `SCOPE`_).
* ``key``: A NULL-terminated string identifying the value. The string must be no
  longer than ``PMIX_MAX_KEYLEN`` characters and must not begin with the reserved
  prefix ``"pmix"``.
* ``val``: Pointer to a ``pmix_value_t`` structure containing the value to be
  posted.


DESCRIPTION
-----------

Post a key-value pair for distribution. The provided value is copied into
internal memory before ``PMIx_Put`` returns, so the caller may modify or free
``val`` immediately afterward. The client PMIx library caches the posted value
locally until :ref:`PMIx_Commit(3) <man3-PMIx_Commit>` is called, at which point
the committed values are pushed to the local PMIx server, which distributes the
data as directed by each value's scope.

The ``pmix_value_t`` structure supports both string and binary values. PMIx
implementations support heterogeneous environments by properly converting binary
values between host architectures.

.. note::
   Keys beginning with the string ``"pmix"`` are reserved for use by the PMIx
   Standard and the library. Applications must never use a defined ``PMIX_``
   attribute |mdash| or any other ``"pmix"``-prefixed string |mdash| as the
   ``key`` in a call to ``PMIx_Put``; doing so returns ``PMIX_ERR_BAD_PARAM``.


SCOPE
-----

The ``pmix_scope_t`` value passed as ``scope`` determines which processes are
able to access the posted data. It is a ``uint8_t`` type taking one of the
following values:

* ``PMIX_LOCAL`` |mdash| the data is intended only for other application
  processes on the same node. It is not included in data packages sent to remote
  requesters.
* ``PMIX_REMOTE`` |mdash| the data is intended solely for application processes on
  remote nodes. It is not shared with other processes on the same node.
* ``PMIX_GLOBAL`` |mdash| the data is to be shared with all other requesting
  processes, regardless of location.
* ``PMIX_INTERNAL`` |mdash| the data is intended solely for this process and is
  not shared with any other process. Typically used to cache data the process
  obtained by means outside of PMIx.

This implementation supports four additional values |mdash| ``PMIX_DEL_LOCAL``,
``PMIX_DEL_REMOTE``, ``PMIX_DEL_GLOBAL`` and ``PMIX_DEL_INTERNAL`` |mdash| which
name the same audiences as the four above but direct that ``key`` be **removed**
rather than stored. ``val`` is ignored for these and may be ``NULL``. Removing a
key that was never stored is not an error: the caller asked for it to be absent,
and it is. As with a store, the removal takes effect on the calling process
immediately and reaches other processes through the usual
:ref:`PMIx_Commit(3) <man3-PMIx_Commit>` and exchange path, so a
``PMIX_DEL_INTERNAL`` |mdash| which was never shared |mdash| is complete on
return.

A specific implementation may support additional scope values, but all
implementations support at least ``PMIX_GLOBAL``. If a specified scope value is
not supported, ``PMIx_Put`` returns ``PMIX_ERR_NOT_SUPPORTED``. That is what a
delete scope returns here when the local PMIx server is too old to act on it,
which is checked up front rather than left to fail silently at the server.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``key`` is ``NULL``, exceeds
  ``PMIX_MAX_KEYLEN``, or uses the reserved ``"pmix"`` prefix.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the requested ``scope`` is not supported by
  the implementation.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_Put`` only stages data locally; the values are not made available to other
processes until they are committed with :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`
and, typically, a subsequent synchronization such as
:ref:`PMIx_Fence(3) <man3-PMIx_Fence>` has completed.

**Posting a new value under a key already published** is permitted, but note
that peers which have already retrieved that key will continue to see the
earlier value: retrieved data is cached locally, and a later exchange does not
by itself invalidate it. Such a peer must ask for the update with
``PMIX_GET_REFRESH_CACHE`` |mdash| see
:ref:`PMIx_Get(3) <man3-PMIx_Get>`.

**Deleting a key.** The removal requested by a ``PMIX_DEL_*`` scope is applied
to the calling process at once. The local PMIx server, and every other client
of that server that had already been handed the key, are corrected when the
removal is committed with :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`; the server
tells them with a one-way notification that carries no acknowledgement, so
that correction arrives promptly rather than synchronously and a peer that
races it can see the old value once more. Processes on *other* nodes stop
seeing the key at the next collecting
:ref:`PMIx_Fence(3) <man3-PMIx_Fence>` |mdash| the exchange is additive, so the
removal has to be stated there, and a barrier-only fence does not carry it.

Within a single commit interval the server applies the removals before the
values published alongside them, so a key that is deleted and then posted
again before the next ``PMIx_Commit`` ends up **present**, while one that is
posted and then deleted ends up **absent**.

**Qualified values.** A value may be posted together with one or more qualifiers
that scope its later retrieval by using the reserved key
``PMIX_QUALIFIED_VALUE``. In that case ``val`` must be a ``pmix_value_t`` of type
``PMIX_DATA_ARRAY`` whose first element is the primary key-value pair and whose
remaining elements are the qualifier key-value pairs. The stored value is later
obtained with :ref:`PMIx_Get(3) <man3-PMIx_Get>` by supplying the matching
qualifiers, allowing several distinct values to be posted under the same key.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_scope_t(5) <man5-pmix_scope_t>`,
   :doc:`Modex: Exchanging Process Data </how-things-work/modex>`
