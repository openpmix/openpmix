.. _man3-PMIx_App_destruct:

PMIx_App_destruct
=================

.. include_body

``PMIx_App_destruct`` |mdash| Release the contents of a
:ref:`pmix_app_t(5) <man5-pmix_app_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_App_destruct(pmix_app_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_app_t(5) <man5-pmix_app_t>` structure whose
  contents are to be released.


DESCRIPTION
-----------

Release all heap memory referenced by the fields of a
:ref:`pmix_app_t <man5-pmix_app_t>` structure. The ``cmd`` and ``cwd`` strings
are freed, the ``argv`` and ``env`` string arrays are freed, and the ``info``
array (if present and ``ninfo`` is greater than zero) is freed. Each freed
field pointer is
reset to ``NULL`` and ``ninfo`` is reset to zero, leaving the structure safe to
reuse or destruct again.

``PMIx_App_destruct`` does **not** free the storage of the ``pmix_app_t``
structure itself. It releases only the payload attached to a structure that was
previously initialized with :ref:`PMIx_App_construct(3)
<man3-PMIx_App_construct>`.


RETURN VALUE
------------

``PMIx_App_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_App_destruct`` is the counterpart of :ref:`PMIx_App_construct(3)
<man3-PMIx_App_construct>`. To release an app array that was heap-allocated by
:ref:`PMIx_App_create(3) <man3-PMIx_App_create>`, use :ref:`PMIx_App_free(3)
<man3-PMIx_App_free>` (which destructs each element and frees the array) or
:ref:`PMIx_App_release(3) <man3-PMIx_App_release>` for a single app.


.. seealso::
   :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`,
   :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`,
   :ref:`PMIx_App_info_create(3) <man3-PMIx_App_info_create>`,
   :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`,
   :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`
