.. _man3-PMIx_Info_persistent:

PMIx_Info_persistent
====================

.. include_body

``PMIx_Info_persistent`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as persistent


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_persistent(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked persistent.


DESCRIPTION
-----------

The ``PMIx_Info_persistent`` function sets the ``PMIX_INFO_PERSISTENT`` directive
bit in the info structure's flags field. A persistent info is understood **not**
to own the :ref:`pmix_value_t(5) <man5-pmix_value_t>` payload it carries, so the
library will not release that value on the info's behalf.

This flag changes the behavior of the info lifecycle routines:

* :ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>` and
  :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>` leave the contained value
  untouched instead of releasing it.
* :ref:`PMIx_Info_xfer(3) <man3-PMIx_Info_xfer>` copies the value by shallow
  structure assignment (the destination shares the source's underlying value
  memory) rather than deep-copying it.

Use this flag when the value memory is owned elsewhere and must outlive the info
structure, to avoid a double free. Whether an info is currently persistent can
be tested with :ref:`PMIx_Info_is_persistent(3) <man3-PMIx_Info_is_persistent>`.


RETURN VALUE
------------

``PMIx_Info_persistent`` returns no value (``void``).


NOTES
-----

``PMIx_Info_persistent`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_SET_PERSISTENT`` macro. That macro is retained in
``pmix_deprecated.h`` for backward compatibility and simply expands to a call to
this function.

Info structures default to non-persistent at construction, which the surrounding
source notes is retained "for historical reasons": the library owns and releases
the contained value unless this flag is explicitly set.


.. seealso::
   :ref:`PMIx_Info_is_persistent(3) <man3-PMIx_Info_is_persistent>`,
   :ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>`,
   :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>`,
   :ref:`PMIx_Info_xfer(3) <man3-PMIx_Info_xfer>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
