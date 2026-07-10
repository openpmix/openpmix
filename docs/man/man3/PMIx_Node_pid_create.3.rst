.. _man3-PMIx_Node_pid_create:

PMIx_Node_pid_create
====================

.. include_body

``PMIx_Node_pid_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_node_pid_t* PMIx_Node_pid_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>` structures
  to allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_node_pid_t
<man5-pmix_node_pid_t>` structures on the heap and construct (initialize) each
element as if by :ref:`PMIx_Node_pid_construct(3)
<man3-PMIx_Node_pid_construct>`.

If ``n`` is zero, the function returns ``NULL`` without allocating. If the
underlying allocation fails, ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated, constructed array of
``pmix_node_pid_t`` structures, or ``NULL`` if ``n`` is zero or the allocation
could not be satisfied.


NOTES
-----

The returned array is owned by the caller and must be released with
:ref:`PMIx_Node_pid_free(3) <man3-PMIx_Node_pid_free>`, passing the same count
``n`` that was used to create it. ``PMIx_Node_pid_free`` both destructs the
contents of each element and frees the array storage itself.


EXAMPLES
--------

Create and later free an array:

.. code-block:: c

    pmix_node_pid_t *array;

    array = PMIx_Node_pid_create(4);
    if (NULL == array) {
        /* handle allocation failure */
    }

    /* ... use the array ... */

    PMIx_Node_pid_free(array, 4);


.. seealso::
   :ref:`PMIx_Node_pid_free(3) <man3-PMIx_Node_pid_free>`,
   :ref:`PMIx_Node_pid_construct(3) <man3-PMIx_Node_pid_construct>`,
   :ref:`PMIx_Node_pid_destruct(3) <man3-PMIx_Node_pid_destruct>`,
   :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
