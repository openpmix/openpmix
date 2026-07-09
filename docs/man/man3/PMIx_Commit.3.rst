.. _man3-PMIx_Commit:

PMIx_Commit
===========

.. include_body

``PMIx_Commit`` |mdash| Make previously staged key/value pairs available to other
processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Commit(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after one or more foo.put() calls ...
  rc = foo.commit()


DESCRIPTION
-----------

Make available to other processes all key-value pairs previously staged by this
process via :ref:`PMIx_Put(3) <man3-PMIx_Put>`.

A PMIx implementation may locally cache non-reserved keys in the client library
until they are committed. ``PMIx_Commit`` initiates the operation of making those
staged key-value pairs available; depending on the implementation, this may
involve transmitting the entire collection of data posted by the process to the
local PMIx server. ``PMIx_Commit`` is an asynchronous operation: it returns to the
caller immediately while the data is staged to the server in the background.

.. note::
   Users are advised to always include the call to ``PMIx_Commit`` in case the
   local implementation requires it. Committing does not by itself circulate the
   posted data to other processes: availability of the data to peers still relies
   on the exchange mechanisms |mdash| typically a synchronization such as
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>` |mdash| that govern data sharing.

``PMIx_Commit`` takes no arguments. As a convenience, it is a no-op that returns
``PMIX_SUCCESS`` when there is nothing to commit |mdash| for example, when the
caller is a singleton process with no local server to receive the data.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Because commitment does not guarantee that peers can immediately retrieve the
data, a typical publication sequence is one or more
:ref:`PMIx_Put(3) <man3-PMIx_Put>` calls, followed by ``PMIx_Commit``, followed by
a collective synchronization such as :ref:`PMIx_Fence(3) <man3-PMIx_Fence>` before
the peers issue :ref:`PMIx_Get(3) <man3-PMIx_Get>`.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
