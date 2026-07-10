.. _man3-PMIx_Procid_invalid:

PMIx_Procid_invalid
===================

.. include_body

``PMIx_Procid_invalid`` |mdash| Test whether a
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` is invalid


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Procid_invalid(const pmix_proc_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` process
  identifier to be tested.


DESCRIPTION
-----------

Test whether a process identifier is invalid. A
:ref:`pmix_proc_t <man5-pmix_proc_t>` is considered invalid if *either* its
namespace is invalid (``NULL`` or empty, as determined by
:ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`) *or* its rank is
equal to ``PMIX_RANK_INVALID``. A process identifier is valid only when both
its namespace and its rank are valid.


RETURN VALUE
------------

Returns ``true`` if the process identifier is invalid (invalid namespace or
``PMIX_RANK_INVALID`` rank), and ``false`` otherwise.


NOTES
-----

``PMIx_Procid_invalid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_PROCID_INVALID`` macro. The argument must point
to a valid structure; the routine dereferences ``p`` and performs no ``NULL``
checking on the pointer itself.


.. seealso::
   :ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`,
   :ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`,
   :ref:`PMIx_Load_procid(3) <man3-PMIx_Load_procid>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
