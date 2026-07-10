.. _man3-PMIx_Load_procid:

PMIx_Load_procid
================

.. include_body

``PMIx_Load_procid`` |mdash| Populate a
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` with a namespace and rank


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Load_procid(pmix_proc_t *p,
                         const char *ns,
                         pmix_rank_t rk);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure to
  be populated. The storage must already exist |mdash| it is supplied by the
  caller.
* ``ns``: Pointer to a NULL-terminated namespace string to load into the
  structure's ``nspace`` field. May be ``NULL``.
* ``rk``: The :ref:`pmix_rank_t(5) <man5-pmix_rank_t>` value to store in the
  structure's ``rank`` field.


DESCRIPTION
-----------

Populate the fields of a :ref:`pmix_proc_t <man5-pmix_proc_t>` process
identifier. The ``nspace`` field is loaded from ``ns`` using the same
semantics as :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`: the field is
first cleared, then up to ``PMIX_MAX_NSLEN`` characters are copied from
``ns`` (or the field is left empty if ``ns`` is ``NULL``). The ``rank`` field
is then set to ``rk``.

Unlike :ref:`PMIx_Proc_load(3) <man3-PMIx_Proc_load>`, which first constructs
the structure (setting the rank to ``PMIX_RANK_UNDEF``) before loading, this
routine writes only the ``nspace`` and ``rank`` fields directly.


RETURN VALUE
------------

``PMIx_Load_procid`` returns no value (``void``).


NOTES
-----

``PMIx_Load_procid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_LOAD_PROCID`` macro.


.. seealso::
   :ref:`PMIx_Proc_load(3) <man3-PMIx_Proc_load>`,
   :ref:`PMIx_Xfer_procid(3) <man3-PMIx_Xfer_procid>`,
   :ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`,
   :ref:`PMIx_Procid_invalid(3) <man3-PMIx_Procid_invalid>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`,
   :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`
