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


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   pinfo = {'proc': {'nspace': "myjob", 'rank': 0}, 'hostname': "node01",
            'executable': "/bin/hostname", 'pid': 12345, 'exitcode': 0,
            'state': PMIX_PROC_STATE_RUNNING}

where ``proc`` is a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` dict,
``hostname`` and ``executable`` are strings (``None`` for NULL), ``pid``
is a non-negative process id, ``exitcode`` an ``int`` and ``state`` a
:ref:`pmix_proc_state_t(5) <man5-pmix_proc_state_t>` value. Note the names
``executable`` and ``exitcode``, which differ from the C members. All six
must be present. A proc info handed back by the library has the same
shape.


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

STATIC INITIALIZER
------------------

A statically declared ``pmix_proc_info_t`` may be initialized with the
``PMIX_PROC_INFO_STATIC_INIT`` macro, which initializes the embedded ``proc`` (with ``rank`` set to ``PMIX_RANK_UNDEF``), sets ``hostname`` and ``executable_name`` to ``NULL`` and ``pid`` and ``exit_code`` to ``0``, and sets ``state`` to ``PMIX_PROC_STATE_UNDEF``:

.. code-block:: c

   pmix_proc_info_t pinfo = PMIX_PROC_INFO_STATIC_INIT;


.. seealso::
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_proc_state_t(5) <man5-pmix_proc_state_t>`
