.. _man3-PMIx_Regattr_free:

PMIx_Regattr_free
=================

.. include_body

``PMIx_Regattr_free`` |mdash| Release an array of
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structures and their contents.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regattr_free(pmix_regattr_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_regattr_t <man5-pmix_regattr_t>` structures that
was allocated by :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`. The
function first destructs each of the ``n`` elements |mdash| freeing their
``name`` and ``description`` contents, exactly as
:ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>` would |mdash| and
then frees the storage of the array itself.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.


RETURN VALUE
------------

``PMIx_Regattr_free`` returns no value (``void``).


NOTES
-----

The ``PMIX_REGATTR_FREE`` convenience macro is a direct wrapper for this
function; in addition to calling it, the macro sets the caller's pointer to
``NULL``.

Use ``PMIx_Regattr_free`` only on storage obtained from
:ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`. For a caller-provided
(stack or embedded) structure, use :ref:`PMIx_Regattr_destruct(3)
<man3-PMIx_Regattr_destruct>` instead, which releases the contents without
freeing the structure storage.


.. seealso::
   :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`,
   :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`,
   :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`,
   :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`,
   :ref:`PMIx_Regattr_xfer(3) <man3-PMIx_Regattr_xfer>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
