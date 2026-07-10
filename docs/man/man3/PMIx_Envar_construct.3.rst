.. _man3-PMIx_Envar_construct:

PMIx_Envar_construct
====================

.. include_body

``PMIx_Envar_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Envar_construct(pmix_envar_t *e);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the :ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structure to
  be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the fields of a :ref:`pmix_envar_t <man5-pmix_envar_t>` that the
caller has already allocated. The ``envar`` and ``value`` string pointers are
set to ``NULL`` and the ``separator`` character is set to ``'\0'``. No heap
memory is allocated.

A structure must be constructed (or created with
:ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`) before its contents are
populated |mdash| for example with :ref:`PMIx_Envar_load(3)
<man3-PMIx_Envar_load>` |mdash| or used in any operation.

Because ``PMIx_Envar_construct`` operates on caller-provided storage, it must be
paired with :ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>` to release
any strings the structure subsequently acquires. Do **not** pass a constructed
(as opposed to created) structure to :ref:`PMIx_Envar_free(3)
<man3-PMIx_Envar_free>`, as that would attempt to free storage the library did
not allocate.


RETURN VALUE
------------

``PMIx_Envar_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Envar_construct`` is an OpenPMIx convenience routine. The equivalent
static initializer ``PMIX_ENVAR_STATIC_INIT`` may be used where a compile-time
initialization is preferred.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_envar_t envar;

    PMIx_Envar_construct(&envar);


.. seealso::
   :ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>`,
   :ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`,
   :ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>`,
   :ref:`PMIx_Envar_load(3) <man3-PMIx_Envar_load>`,
   :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
