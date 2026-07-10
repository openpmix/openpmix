.. _man3-PMIx_Fabric_construct:

PMIx_Fabric_construct
=====================

.. include_body

``PMIx_Fabric_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Fabric_construct(pmix_fabric_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>` structure
  to be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the fields of a :ref:`pmix_fabric_t <man5-pmix_fabric_t>` that the
caller has already allocated, placing it in a well-defined state before it is
passed to :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`.
Specifically:

* ``name`` is set to ``NULL``;
* ``index`` is set to ``SIZE_MAX`` (indicating that no fabric registration
  object has yet been assigned by the PMIx library);
* ``info`` is set to ``NULL`` and ``ninfo`` to zero;
* ``module`` is set to ``NULL``.

No heap memory is allocated. A caller that wishes to register a specific fabric
by name may set the ``name`` field after construction and before calling
:ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`; leaving it ``NULL``
requests information on the default fabric.


RETURN VALUE
------------

``PMIx_Fabric_construct`` returns no value (``void``).


NOTES
-----

The ``info`` array and other internal fields of a :ref:`pmix_fabric_t
<man5-pmix_fabric_t>` are populated by the PMIx library when the structure is
registered with :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`, and
are released when the structure is deregistered with
:ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`. A constructed
but never-registered structure holds no library-allocated resources.


EXAMPLES
--------

Construct and register a fabric object:

.. code-block:: c

    pmix_fabric_t fabric;

    PMIx_Fabric_construct(&fabric);
    PMIx_Fabric_register(&fabric, NULL, 0);


.. seealso::
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`,
   :ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`
