.. _man3-PMIx_Info_list_get_info:

PMIx_Info_list_get_info
=======================

.. include_body

``PMIx_Info_list_get_info`` |mdash| Iterate over the
:ref:`pmix_info_t(5) <man5-pmix_info_t>` entries of an opaque list


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_info_t* PMIx_Info_list_get_info(void *ptr, void *prev, void **next);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``ptr``: (IN) Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``prev``: (IN) Opaque pointer to the current list element, as previously
  returned in ``next``. Pass ``NULL`` to obtain the first element.
* ``next``: (OUT) Set to an opaque pointer to the following list element, or to
  ``NULL`` when the returned element is the last on the list.


DESCRIPTION
-----------

Retrieve a :ref:`pmix_info_t <man5-pmix_info_t>` entry from the list identified
by ``ptr`` and advance an iteration cursor. Passing ``NULL`` for ``prev``
returns the first entry on the list; otherwise ``prev`` must be the opaque
element pointer that a previous call returned through ``next``. On return,
``*next`` is set to the opaque handle of the following element, or to ``NULL``
when the returned element is the last on the list.

Iterate by repeatedly calling ``PMIx_Info_list_get_info``, feeding the ``next``
value from one call as the ``prev`` argument of the following call, until
``*next`` becomes ``NULL``.

The returned :ref:`pmix_info_t <man5-pmix_info_t>` pointer refers to storage
owned by the list; the caller must not free it and must not use it after the
list has been released.


RETURN VALUE
------------

Returns a pointer to the current :ref:`pmix_info_t <man5-pmix_info_t>` entry.


NOTES
-----

The ``prev`` parameter names the *current* element to be returned, and ``next``
receives the *following* element |mdash| ``next`` is the value to feed back as
``prev`` on the subsequent call. This routine performs no locking; the list must
not be modified while an iteration is in progress.


EXAMPLES
--------

.. code-block:: c

    pmix_info_t *ip;
    void *cur, *nxt;

    cur = NULL;
    do {
        ip = PMIx_Info_list_get_info(ilist, cur, &nxt);
        /* use ip ... */
        cur = nxt;
    } while (NULL != nxt);


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_get_size(3) <man3-PMIx_Info_list_get_size>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
