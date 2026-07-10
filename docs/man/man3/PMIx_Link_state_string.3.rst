.. _man3-PMIx_Link_state_string:

PMIx_Link_state_string
======================

.. include_body

``PMIx_Link_state_string`` |mdash| Return the string representation of a
``pmix_link_state_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Link_state_string(pmix_link_state_t state);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  name = foo.link_state_string(PMIX_LINK_UP)


INPUT PARAMETERS
----------------

* ``state``: A fabric link state value of type ``pmix_link_state_t``. Recognized
  values are ``PMIX_LINK_STATE_UNKNOWN``, ``PMIX_LINK_DOWN``, and ``PMIX_LINK_UP``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given fabric
link state. The recognized states map as follows: ``PMIX_LINK_DOWN`` yields
``"INACTIVE"``, ``PMIX_LINK_UP`` yields ``"ACTIVE"``, and
``PMIX_LINK_STATE_UNKNOWN`` yields ``"UNKNOWN"``. Any value that does not
correspond to a recognized link state also returns the fallback string
``"UNKNOWN"``.

The returned pointer refers to constant storage owned by the PMIx library. The
caller must **not** modify or free it.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   ``PMIx_Job_state_string(3)``,
   ``PMIx_Device_type_string(3)``,
   :ref:`pmix_link_state_t(5) <man5-pmix_link_state_t>`
