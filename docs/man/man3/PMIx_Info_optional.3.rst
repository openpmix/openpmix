.. _man3-PMIx_Info_optional:

PMIx_Info_optional
==================

.. include_body

``PMIx_Info_optional`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as optional


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_optional(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked optional.


DESCRIPTION
-----------

The ``PMIx_Info_optional`` function clears the ``PMIX_INFO_REQD`` directive bit
in the info structure's flags field. Marking an info optional tells the PMIx
implementation (and the host environment) that the directive it carries is
advisory: a request may proceed and succeed even if the directive cannot be
honored.

This is the inverse of :ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`,
which sets the same bit. Because an info's flags default to zero at
construction, a freshly constructed info is already optional; this function is
used to return a previously-required info to the optional state. Whether an info
is currently optional can be tested with
:ref:`PMIx_Info_is_optional(3) <man3-PMIx_Info_is_optional>`.


RETURN VALUE
------------

``PMIx_Info_optional`` returns no value (``void``).


NOTES
-----

``PMIx_Info_optional`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_OPTIONAL`` macro. That macro is retained in ``pmix_deprecated.h``
for backward compatibility and simply expands to a call to this function.


.. seealso::
   :ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`,
   :ref:`PMIx_Info_is_optional(3) <man3-PMIx_Info_is_optional>`,
   :ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
