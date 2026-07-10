.. _man3-PMIx_App_free:

PMIx_App_free
=============

.. include_body

``PMIx_App_free`` |mdash| Release an array of
:ref:`pmix_app_t(5) <man5-pmix_app_t>` structures allocated by
:ref:`PMIx_App_create(3) <man3-PMIx_App_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_App_free(pmix_app_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_app_t(5) <man5-pmix_app_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_app_t <man5-pmix_app_t>` structures that was
previously allocated by :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`. Each
of the ``n`` elements is destructed (its payload |mdash| ``cmd``, ``argv``,
``env``, ``cwd``, and ``info`` |mdash| is released) and then the array storage
itself is freed.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.


RETURN VALUE
------------

``PMIx_App_free`` returns no value (``void``).


NOTES
-----

``PMIx_App_free`` is the counterpart of :ref:`PMIx_App_create(3)
<man3-PMIx_App_create>`. The ``n`` passed here must match the count used at
creation. For releasing a single app structure, :ref:`PMIx_App_release(3)
<man3-PMIx_App_release>` is a convenience wrapper equivalent to
``PMIx_App_free(p, 1)``.


.. seealso::
   :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`,
   :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
   :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`,
   :ref:`PMIx_App_info_create(3) <man3-PMIx_App_info_create>`,
   :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`
