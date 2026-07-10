.. _man5-pmix_proc_info_t:

pmix_proc_info_t
================

.. include_body

`pmix_proc_info_t` |mdash| Describes a specific process, including its name,
location, and state

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_proc_info {
       pmix_proc_t proc;
       char *hostname;
       char *executable_name;
       pid_t pid;
       int exit_code;
       pmix_proc_state_t state;
   } pmix_proc_info_t;


DESCRIPTION
-----------

The `pmix_proc_info_t` structure defines a set of information about a specific
process, including its name, location, and state.

The fields are:

* ``proc`` |mdash| The :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` (namespace and
  rank) identifying the process.
* ``hostname`` |mdash| Name of the host (node) where the process resides.
* ``executable_name`` |mdash| Name of the executable the process is running.
* ``pid`` |mdash| Operating system process identifier on the host.
* ``exit_code`` |mdash| Exit code of the process. Defaults to ``0``.
* ``state`` |mdash| Current :ref:`pmix_proc_state_t(5) <man5-pmix_proc_state_t>`
  of the process.

The macro ``PMIX_PROC_INFO_STATIC_INIT`` is provided to statically initialize
the fields of a `pmix_proc_info_t` structure. PMIx also provides the
``PMIx_Proc_info_construct``, ``PMIx_Proc_info_destruct``,
``PMIx_Proc_info_create``, and ``PMIx_Proc_info_free`` support functions for
initializing, releasing, allocating, and freeing these structures.


.. seealso::
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_proc_state_t(5) <man5-pmix_proc_state_t>`
