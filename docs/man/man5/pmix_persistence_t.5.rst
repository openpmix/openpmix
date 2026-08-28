.. _man5-pmix_persistence_t:

pmix_persistence_t
==================

.. include_body

`pmix_persistence_t` |mdash| Defines the retention policy for data published
by a process

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_persistence_t;

   #define PMIX_PERSIST_INDEF          0   // retain until specifically deleted
   #define PMIX_PERSIST_FIRST_READ     1   // delete upon first access
   #define PMIX_PERSIST_PROC           2   // retain until publishing process terminates
   #define PMIX_PERSIST_APP            3   // retain until application terminates
   #define PMIX_PERSIST_SESSION        4   // retain until session/allocation terminates
   #define PMIX_PERSIST_NSPACE         5   // retain until publishing namespace terminates
   #define PMIX_PERSIST_INVALID   UINT8_MAX


Python Syntax
^^^^^^^^^^^^^

None


DESCRIPTION
-----------

The `pmix_persistence_t` type is used to specify how long published data is to
be retained by the datastore before being automatically deleted. It is passed
as the ``PMIX_PERSISTENCE`` attribute to
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>`. Defined values include:

.. note:: The numeric values are identifiers and not an ordering. They are
   assigned in the order the policies were defined, so a larger value does
   not mean a longer retention |mdash| ``PMIX_PERSIST_INDEF`` is zero and
   outlives every other policy, and ``PMIX_PERSIST_NSPACE`` was added after
   ``PMIX_PERSIST_SESSION`` while ending before it. Compare persistence
   values for equality, never for magnitude.

.. list-table:: Persistence Values
   :align: center
   :header-rows: 1

   * - **NAME**
     - **VALUE**
     - **DESCRIPTION**
   * - `PMIX_PERSIST_INDEF`
     - 0
     - Retain the data until it is specifically deleted (e.g., via :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`).
   * - `PMIX_PERSIST_FIRST_READ`
     - 1
     - Delete the data upon its first access.
   * - `PMIX_PERSIST_PROC`
     - 2
     - Retain the data until the publishing process terminates.
   * - `PMIX_PERSIST_APP`
     - 3
     - Retain the data until the application terminates.
   * - `PMIX_PERSIST_SESSION`
     - 4
     - Retain the data until the session/allocation terminates.
   * - `PMIX_PERSIST_NSPACE`
     - 5
     - Retain the data until the publishing process's namespace terminates |mdash| that is, until every application in that job has ended.
   * - `PMIX_PERSIST_INVALID`
     - UINT8_MAX
     - An invalid persistence value, used to indicate that no valid persistence policy has been set.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`pmix_data_range_t(5) <man5-pmix_data_range_t>`
