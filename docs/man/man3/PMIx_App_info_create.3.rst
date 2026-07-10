.. _man3-PMIx_App_info_create:

PMIx_App_info_create
====================

.. include_body

``PMIx_App_info_create`` |mdash| Allocate and attach an array of
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structures to the ``info`` field of a
:ref:`pmix_app_t(5) <man5-pmix_app_t>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_App_info_create(pmix_app_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_app_t(5) <man5-pmix_app_t>` structure whose
  ``info`` array is to be created.
* ``n``: Number of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate an array of ``n`` :ref:`pmix_info_t <man5-pmix_info_t>` structures
(equivalent to :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`), store the
resulting pointer in the ``info`` field of the supplied
:ref:`pmix_app_t <man5-pmix_app_t>`, and set the ``ninfo`` field to ``n``. This
is a convenience routine for populating an app's directive array.


RETURN VALUE
------------

``PMIx_App_info_create`` returns no value (``void``). The result is delivered
by setting ``p->info`` and ``p->ninfo``; ``p->info`` will be ``NULL`` if ``n``
is zero or the allocation fails.


NOTES
-----

The ``info`` array created here is released when the enclosing app is destructed
or freed |mdash| via :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
:ref:`PMIx_App_free(3) <man3-PMIx_App_free>`, or :ref:`PMIx_App_release(3)
<man3-PMIx_App_release>`. Do not separately free ``p->info`` after handing the
app to one of those routines.


.. seealso::
   :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`,
   :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
   :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`,
   :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`,
   :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`,
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
