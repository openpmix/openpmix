.. _man3-PMIx_App_release:

PMIx_App_release
================

.. include_body

``PMIx_App_release`` |mdash| Release a single
:ref:`pmix_app_t(5) <man5-pmix_app_t>` structure allocated by
:ref:`PMIx_App_create(3) <man3-PMIx_App_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_App_release(pmix_app_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the single :ref:`pmix_app_t(5) <man5-pmix_app_t>` structure
  to be released.


DESCRIPTION
-----------

Release a single heap-allocated :ref:`pmix_app_t <man5-pmix_app_t>` structure.
The structure's payload (``cmd``, ``argv``, ``env``, ``cwd``, and ``info``) is
destructed and then the storage for the structure itself is freed.
``PMIx_App_release`` is exactly equivalent to calling ``PMIx_App_free(p, 1)``.

If ``p`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_App_release`` returns no value (``void``).


NOTES
-----

Use ``PMIx_App_release`` only for a structure that the library allocated
(e.g., via :ref:`PMIx_App_create(3) <man3-PMIx_App_create>` with a count of
one). Do not call it on a caller-provided structure that was merely constructed
with :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`; use
:ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>` for that case.


.. seealso::
   :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`,
   :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
   :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`,
   :ref:`PMIx_App_info_create(3) <man3-PMIx_App_info_create>`,
   :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`
