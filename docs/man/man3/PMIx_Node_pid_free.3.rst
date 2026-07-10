.. _man3-PMIx_Node_pid_free:

PMIx_Node_pid_free
==================

.. include_body

``PMIx_Node_pid_free`` |mdash| Release an array of
:ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Node_pid_free(pmix_node_pid_t *d, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to an array of :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
  structures previously returned by :ref:`PMIx_Node_pid_create(3)
  <man3-PMIx_Node_pid_create>`.
* ``n``: Number of structures in the array |mdash| must be the same count that
  was passed to :ref:`PMIx_Node_pid_create(3) <man3-PMIx_Node_pid_create>`.


DESCRIPTION
-----------

Release an array of :ref:`pmix_node_pid_t <man5-pmix_node_pid_t>` structures
that was allocated by :ref:`PMIx_Node_pid_create(3)
<man3-PMIx_Node_pid_create>`. The function destructs the contents of each of
the ``n`` elements |mdash| freeing the ``hostname`` payload as if by
:ref:`PMIx_Node_pid_destruct(3) <man3-PMIx_Node_pid_destruct>` |mdash| and then
frees the array storage itself.

If ``d`` is ``NULL`` the function returns without action.


RETURN VALUE
------------

``PMIx_Node_pid_free`` returns no value (``void``).


NOTES
-----

``PMIx_Node_pid_free`` is the counterpart to :ref:`PMIx_Node_pid_create(3)
<man3-PMIx_Node_pid_create>`. Do not call it on a single structure that was
merely constructed with :ref:`PMIx_Node_pid_construct(3)
<man3-PMIx_Node_pid_construct>`; use :ref:`PMIx_Node_pid_destruct(3)
<man3-PMIx_Node_pid_destruct>` for that case, since the structure storage was
not allocated by the library.


.. seealso::
   :ref:`PMIx_Node_pid_create(3) <man3-PMIx_Node_pid_create>`,
   :ref:`PMIx_Node_pid_construct(3) <man3-PMIx_Node_pid_construct>`,
   :ref:`PMIx_Node_pid_destruct(3) <man3-PMIx_Node_pid_destruct>`,
   :ref:`pmix_node_pid_t(5) <man5-pmix_node_pid_t>`
