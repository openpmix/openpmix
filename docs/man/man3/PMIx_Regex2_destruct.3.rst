.. _man3-PMIx_Regex2_destruct:

PMIx_Regex2_destruct
====================

.. include_body

``PMIx_Regex2_destruct`` |mdash| Release the contents of a
:ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regex2_destruct(pmix_regex2_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the ``pmix_regex2_t`` structure whose contents are to be
  released.


DESCRIPTION
-----------

Release the payload held by a ``pmix_regex2_t``. If the ``type`` pointer is
non-``NULL`` it is freed; likewise, if the ``bytes`` pointer is non-``NULL`` it
is freed. The storage for the structure itself is **not** freed |mdash| that
remains the responsibility of the caller.

``PMIx_Regex2_destruct`` is the counterpart to :ref:`PMIx_Regex2_construct(3)
<man3-PMIx_Regex2_construct>` and should be used to clean up any structure that
was constructed on caller-provided storage.


RETURN VALUE
------------

``PMIx_Regex2_destruct`` returns no value (``void``).


NOTES
-----

To release an array of structures that was obtained from
:ref:`PMIx_Regex2_create(3) <man3-PMIx_Regex2_create>` |mdash| which also frees
the array storage itself |mdash| use :ref:`PMIx_Regex2_free(3)
<man3-PMIx_Regex2_free>` instead.


.. seealso::
   :ref:`PMIx_Regex2_construct(3) <man3-PMIx_Regex2_construct>`,
   :ref:`PMIx_Regex2_create(3) <man3-PMIx_Regex2_create>`,
   :ref:`PMIx_Regex2_free(3) <man3-PMIx_Regex2_free>`,
   :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>`
