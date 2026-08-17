.. _man5-pmix_scope_t:

pmix_scope_t
============

.. include_body

`pmix_scope_t` |mdash| Defines the scope of visibility for data "put" by a process

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_scope_t;

   #define PMIX_SCOPE_UNDEF    0
   #define PMIX_LOCAL          1   // share to procs also on this node
   #define PMIX_REMOTE         2   // share with procs not on this node
   #define PMIX_GLOBAL         3   // share with all procs (local + remote)
   #define PMIX_INTERNAL       4   // store data in the internal tables
   #define PMIX_DEL_LOCAL      5   // remove from the local scope
   #define PMIX_DEL_REMOTE     6   // remove from the remote scope
   #define PMIX_DEL_GLOBAL     7   // remove from both local and remote
   #define PMIX_DEL_INTERNAL   8   // remove from the internal tables


Python Syntax
^^^^^^^^^^^^^

None


DESCRIPTION
-----------

The `pmix_scope_t` type is used to specify the scope of visibility of data
committed via :ref:`PMIx_Put(3) <man3-PMIx_Put>` |mdash| that is, the set of
processes to which the data is to be made available. Defined values include:

.. list-table:: Scope Values
   :align: center
   :header-rows: 1

   * - **NAME**
     - **VALUE**
     - **DESCRIPTION**
   * - `PMIX_SCOPE_UNDEF`
     - 0
     - No scope has been defined.
   * - `PMIX_LOCAL`
     - 1
     - The data is intended only for other application processes on the same node. Data marked in this way will not be included in data packages sent to remote requestors.
   * - `PMIX_REMOTE`
     - 2
     - The data is intended solely for application processes on remote nodes. Data marked in this way will not be shared with other processes on the same node.
   * - `PMIX_GLOBAL`
     - 3
     - The data is to be shared with all other requesting processes, regardless of location (both local and remote).
   * - `PMIX_INTERNAL`
     - 4
     - The data is to be stored in the internal tables of the local PMIx library and is not to be shared with any other process.
   * - `PMIX_DEL_LOCAL`
     - 5
     - Remove the key from the local scope. The audience is the same as ``PMIX_LOCAL``; the key is removed rather than stored.
   * - `PMIX_DEL_REMOTE`
     - 6
     - Remove the key from the remote scope. The audience is the same as ``PMIX_REMOTE``; the key is removed rather than stored.
   * - `PMIX_DEL_GLOBAL`
     - 7
     - Remove the key from both the local and remote scopes.
   * - `PMIX_DEL_INTERNAL`
     - 8
     - Remove the key from the internal tables of the local PMIx library. Nothing was shared, so this takes effect immediately and involves no other process.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
