.. _man3-PMIx_Info_relayed:

PMIx_Info_relayed
=================

.. include_body

``PMIx_Info_relayed`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
as an identity being relayed for the process that made a request.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_relayed(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked as relayed.


DESCRIPTION
-----------

The ``PMIx_Info_relayed`` function sets the ``PMIX_INFO_RELAYED`` directive bit
in the info structure's flags field.

It is meant for a ``PMIX_USERID`` or ``PMIX_GRPID`` that a PMIx server is passing
on for someone else. When a server's host relays a request - forwarding what one
of its own clients or tools asked for to another server - the ``PMIX_USERID``
and ``PMIX_GRPID`` it was handed are those of the process that made the request.
The server where that request entered set them from what it authenticated. The
host marks them as relayed when it passes them on. The receiving server keeps
them, still marked, rather than replacing them with the relaying process's own
identity - so the operation is attributed to the process that asked for it.

The mark is honored only when a PMIx server library sends it: any other process's
library clears it, so a process that is not a server speaks only for itself. It
has no meaning on any other attribute.

Whether an info is marked as relayed can be tested with
:ref:`PMIx_Info_is_relayed(3) <man3-PMIx_Info_is_relayed>`.


RETURN VALUE
------------

``PMIx_Info_relayed`` returns no value (``void``).


NOTES
-----

A server that does not know the mark - one from an older release - replaces the
values as usual, so the relayed request is attributed to the relaying process.
The ``PMIX_CAP_INFO_RELAYED`` capability flag says the library supports it.


.. seealso::
   :ref:`PMIx_Info_is_relayed(3) <man3-PMIx_Info_is_relayed>`,
   :ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
