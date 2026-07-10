.. _man3-PMIx_Node_pid_construct:

PMIx_Node_pid_construct
=======================

.. include_body

``PMIx_Node_pid_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Node_pid_construct(pmix_node_pid_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on the
  stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_node_pid_t <man5-pmix_node_pid_t>` that
the caller has already allocated. The function sets the ``hostname`` pointer to
``NULL``, the ``nodeid`` field to ``UINT32_MAX``, and the ``pid`` field to
``-1``.

No heap memory is allocated; the caller is responsible for populating the
fields after construction.


RETURN VALUE
------------

``PMIx_Node_pid_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Node_pid_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Node_pid_destruct(3) <man3-PMIx_Node_pid_destruct>`
to release any ``hostname`` payload the structure subsequently acquires. Do
**not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Node_pid_free(3) <man3-PMIx_Node_pid_free>`, as that would attempt
to free storage the library did not allocate.

An equivalent static initializer, ``PMIX_NODE_PID_STATIC_INIT``, is available
for initializing a structure at the point of declaration.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_node_pid_t np;

    PMIx_Node_pid_construct(&np);


.. seealso::
   :ref:`PMIx_Node_pid_destruct(3) <man3-PMIx_Node_pid_destruct>`,
   :ref:`PMIx_Node_pid_create(3) <man3-PMIx_Node_pid_create>`,
   :ref:`PMIx_Node_pid_free(3) <man3-PMIx_Node_pid_free>`,
   :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
