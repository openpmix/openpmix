.. _man3-PMIx_Info_xfer:

PMIx_Info_xfer
==============

.. include_body

``PMIx_Info_xfer`` |mdash| Copy the contents of one :ref:`pmix_info_t(5) <man5-pmix_info_t>` into another


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_xfer(pmix_info_t *dest,
                                const pmix_info_t *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dest``: Pointer to the destination :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structure that will receive the copied key, flags, and value. Must not be
  ``NULL``.


INPUT PARAMETERS
----------------

* ``src``: Pointer to the source :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structure. Must not be ``NULL``. The source is not modified.


DESCRIPTION
-----------

The ``PMIx_Info_xfer`` function transfers the contents of ``src`` into
``dest``. Despite the name, the operation is a **copy**: the source structure is
left unchanged. The key and the directive flags of ``src`` are copied into
``dest``, and the contained :ref:`pmix_value_t(5) <man5-pmix_value_t>` is
duplicated so that ``dest`` holds its own independent copy of the payload.

There is one exception governed by the persistent flag: if ``src`` is marked
persistent (see :ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`),
its value is copied by shallow structure assignment rather than deep-copied.
Because a persistent info does not own its value, ``dest`` then shares the same
underlying value memory as ``src`` and must not release it.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, one of the negative PMIx error
constants is returned, including:

* ``PMIX_ERR_BAD_PARAM``: ``dest`` or ``src`` was ``NULL``.

Other errors (for example ``PMIX_ERR_NOMEM``) may be propagated from the
underlying value-copy operation.


NOTES
-----

``PMIx_Info_xfer`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_XFER`` macro. That macro is retained in ``pmix_deprecated.h`` for
backward compatibility and expands to a call to this function, discarding the
return status.

The destination structure is overwritten in place; if it previously held a live
payload, that payload is not released by this call and should be destructed
beforehand to avoid a leak.


.. seealso::
   :ref:`PMIx_Info_load(3) <man3-PMIx_Info_load>`,
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>`,
   :ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
