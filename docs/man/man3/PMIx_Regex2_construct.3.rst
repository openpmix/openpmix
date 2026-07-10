.. _man3-PMIx_Regex2_construct:

PMIx_Regex2_construct
=====================

.. include_body

``PMIx_Regex2_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regex2_construct(pmix_regex2_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the ``pmix_regex2_t`` structure to be initialized. The
  storage for the structure itself must already exist |mdash| it is supplied by
  the caller (typically declared on the stack or embedded within another
  object).


DESCRIPTION
-----------

Initialize the fields of a ``pmix_regex2_t`` that the caller has already
allocated. A ``pmix_regex2_t`` describes a PMIx encoded regular expression and
carries three fields:

* ``type`` |mdash| a colon-delimited identifier of the encoding method (for
  example ``"pmix"`` or ``"blob"``);
* ``bytes`` |mdash| the encoded representation, which may not be
  ``NULL``-terminated;
* ``len`` |mdash| the number of bytes in the encoded representation.

Construction sets the ``type`` and ``bytes`` pointers to ``NULL`` and ``len``
to zero. No heap memory is allocated.

Because ``PMIx_Regex2_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Regex2_destruct(3) <man3-PMIx_Regex2_destruct>` to
release any payload the structure subsequently acquires. Do **not** pass a
constructed (as opposed to created) structure to :ref:`PMIx_Regex2_free(3)
<man3-PMIx_Regex2_free>`, as that would attempt to free storage the library did
not allocate.


RETURN VALUE
------------

``PMIx_Regex2_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Regex2_construct`` is an OpenPMIx convenience routine. The equivalent
static initializer ``PMIX_REGEX2_STATIC_INIT`` may be used where a compile-time
initialization is preferred.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_regex2_t rex;

    PMIx_Regex2_construct(&rex);


.. seealso::
   :ref:`PMIx_Regex2_destruct(3) <man3-PMIx_Regex2_destruct>`,
   :ref:`PMIx_Regex2_create(3) <man3-PMIx_Regex2_create>`,
   :ref:`PMIx_Regex2_free(3) <man3-PMIx_Regex2_free>`,
   :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>`
