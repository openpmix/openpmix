.. _man3-PMIx_Node_pid_destruct:

PMIx_Node_pid_destruct
======================

.. include_body

``PMIx_Node_pid_destruct`` |mdash| Release the contents of a
:ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Node_pid_destruct(pmix_node_pid_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the dynamically allocated payload held within a
:ref:`pmix_node_pid_t <man5-pmix_node_pid_t>` structure. The function frees the
``hostname`` string if one is present.

``PMIx_Node_pid_destruct`` releases only the *contents* of the structure; it
does **not** free the storage of the ``pmix_node_pid_t`` structure itself. When
the structure was allocated on the stack or embedded in another object, that
storage is reclaimed by its owner.


RETURN VALUE
------------

``PMIx_Node_pid_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Node_pid_destruct`` is the counterpart to :ref:`PMIx_Node_pid_construct(3)
<man3-PMIx_Node_pid_construct>`. To release an array of structures that was
allocated by :ref:`PMIx_Node_pid_create(3) <man3-PMIx_Node_pid_create>`,
including the array storage itself, use :ref:`PMIx_Node_pid_free(3)
<man3-PMIx_Node_pid_free>` instead.


.. seealso::
   :ref:`PMIx_Node_pid_construct(3) <man3-PMIx_Node_pid_construct>`,
   :ref:`PMIx_Node_pid_create(3) <man3-PMIx_Node_pid_create>`,
   :ref:`PMIx_Node_pid_free(3) <man3-PMIx_Node_pid_free>`,
   :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
