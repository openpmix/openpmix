.. _man3-PMIx_Info_is_relayed:

PMIx_Info_is_relayed
====================

.. include_body

``PMIx_Info_is_relayed`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
is marked as relayed.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_relayed(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_RELAYED`` flag is set in the ``flags`` field of the
referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. A host receiving an
up-call can use it to tell a ``PMIX_USERID`` or ``PMIX_GRPID`` relayed by another
server - the identity of the process that made the request - from one the
library took from the connection the request arrived on.

The flag is applied with :ref:`PMIx_Info_relayed(3) <man3-PMIx_Info_relayed>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_RELAYED`` flag is set in the structure, and
``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_relayed(3) <man3-PMIx_Info_relayed>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
