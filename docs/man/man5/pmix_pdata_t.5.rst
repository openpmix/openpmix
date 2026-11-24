.. _man5-pmix_pdata_t:

pmix_pdata_t
============

.. include_body

`pmix_pdata_t` |mdash| Describes published data returned by a lookup

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_pdata {
       pmix_proc_t proc;
       pmix_key_t key;
       pmix_value_t value;
   } pmix_pdata_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   pdata = {'proc': {'nspace': "myjob", 'rank': 0}, 'key': "mykey",
            'value': 42, 'val_type': PMIX_INT}

where ``proc`` is the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` dict of the
publisher, ``key`` the published key, and the ``value``/``val_type`` pair
the Python version of a :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
carried directly in the dict. ``lookup()`` takes a list of dicts naming
only the ``key`` of each and returns a list of full pdata dicts; a key it
did not find comes back with ``val_type`` ``PMIX_UNDEF`` and a ``value``
of ``None``. A server's ``lookup`` handler answers with full dicts, and
there every member must be present.


DESCRIPTION
-----------

The `pmix_pdata_t` structure is used by
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>` to describe the data being accessed. A
caller populates the ``key`` field of each element with the string key it wishes
to retrieve; on successful return, the ``proc`` and ``value`` fields are filled
in with the identity of the publisher and the associated value.

* ``proc`` |mdash| the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` process
  identifier of the process that published the data.
* ``key`` |mdash| the :ref:`pmix_key_t(5) <man5-pmix_key_t>` string key under
  which the data was published.
* ``value`` |mdash| the :ref:`pmix_value_t(5) <man5-pmix_value_t>` holding the
  value associated with ``key``.

Data is placed into the PMIx datastore with
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>` and later removed with
:ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`.

STATIC INITIALIZER
------------------

A statically declared ``pmix_pdata_t`` may be initialized with the
``PMIX_LOOKUP_STATIC_INIT`` macro, which initializes the embedded ``proc`` (with ``rank`` set to ``PMIX_RANK_UNDEF``), clears ``key``, and initializes the embedded ``value`` (with ``type`` set to ``PMIX_UNDEF``):

.. code-block:: c

   pmix_pdata_t pdata = PMIX_LOOKUP_STATIC_INIT;

For historical reasons this macro is named ``PMIX_LOOKUP_STATIC_INIT`` rather than ``PMIX_PDATA_STATIC_INIT``.


.. seealso::
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
