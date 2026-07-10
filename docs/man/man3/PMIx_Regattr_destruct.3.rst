.. _man3-PMIx_Regattr_destruct:

PMIx_Regattr_destruct
=====================

.. include_body

``PMIx_Regattr_destruct`` |mdash| Release the contents of a
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regattr_destruct(pmix_regattr_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the dynamically allocated contents of a
:ref:`pmix_regattr_t <man5-pmix_regattr_t>` structure. The function frees the
``name`` string (if present) and frees the ``description`` string array (if
present), resetting both pointers to ``NULL``. The fixed-size ``string`` key
and the ``type`` field require no separate release.

``PMIx_Regattr_destruct`` releases only the payload the structure references; it
does **not** free the storage of the ``pmix_regattr_t`` structure itself. It is
therefore the correct counterpart to
:ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>` for structures
whose storage was supplied by the caller (stack or embedded).

If ``p`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_Regattr_destruct`` returns no value (``void``).


NOTES
-----

The ``PMIX_REGATTR_DESTRUCT`` convenience macro is a direct wrapper for this
function.

To release an array of registration-attribute structures allocated by
:ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>` |mdash| including the
array storage itself |mdash| use :ref:`PMIx_Regattr_free(3)
<man3-PMIx_Regattr_free>` instead.


.. seealso::
   :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`,
   :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`,
   :ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`,
   :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`,
   :ref:`PMIx_Regattr_xfer(3) <man3-PMIx_Regattr_xfer>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
