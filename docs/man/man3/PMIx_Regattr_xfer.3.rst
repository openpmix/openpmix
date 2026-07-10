.. _man3-PMIx_Regattr_xfer:

PMIx_Regattr_xfer
=================

.. include_body

``PMIx_Regattr_xfer`` |mdash| Copy the contents of one
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structure into another.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regattr_xfer(pmix_regattr_t *dest,
                          const pmix_regattr_t *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dest``: Pointer to the destination :ref:`pmix_regattr_t(5)
  <man5-pmix_regattr_t>` structure. Its storage must already exist; it is
  (re)initialized by this call and then populated with copies of the source
  fields.
* ``src``: Pointer to the source :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
  structure whose fields are copied.


DESCRIPTION
-----------

Perform a deep copy of the ``src`` :ref:`pmix_regattr_t <man5-pmix_regattr_t>`
into ``dest``. The function first constructs ``dest`` (as
:ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>` would), then
copies each field: the ``name`` string is duplicated, the ``string`` key is
copied, the ``type`` is assigned, and the ``description`` string array is
duplicated element by element. Fields that are ``NULL`` in the source are left
in their constructed state in the destination.

Because every referenced string is duplicated, ``dest`` and ``src`` share no
storage after the call; each must be released independently with
:ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>` (or
:ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`).


RETURN VALUE
------------

``PMIx_Regattr_xfer`` returns no value (``void``).


NOTES
-----

The ``PMIX_REGATTR_XFER`` convenience macro is a direct wrapper for this
function.

``PMIx_Regattr_xfer`` reinitializes ``dest`` before copying; if ``dest`` already
holds allocated contents, destruct it first (with
:ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`) to avoid leaking
that storage.


.. seealso::
   :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`,
   :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`,
   :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`,
   :ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`,
   :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
