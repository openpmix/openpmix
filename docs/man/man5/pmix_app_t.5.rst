.. _man5-pmix_app_t:

pmix_app_t
==========

.. include_body

`pmix_app_t` |mdash| Describes an application context for spawn operations

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_app {
       char *cmd;
       char **argv;
       char **env;
       char *cwd;
       int maxprocs;
       pmix_info_t *info;
       size_t ninfo;
   } pmix_app_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   app = {'cmd': "/bin/hostname", 'argv': ["hostname", "-s"],
          'env': ["FOO=bar"], 'cwd': "/tmp", 'maxprocs': 2,
          'info': [{'key': PMIX_MAPBY, 'value': "slot", 'val_type': PMIX_STRING}]}

where ``cmd`` is the executable, ``argv`` and ``env`` are lists of
strings, ``cwd`` is the working directory, ``maxprocs`` is the number of
processes, and ``info`` is a list of :ref:`pmix_info_t(5)
<man5-pmix_info_t>` dicts. Only ``cmd`` or a non-empty ``argv`` is
required: a missing ``argv`` is ``[cmd]``, a missing ``cwd`` is the
calling process's current directory, and a missing ``maxprocs`` is 1. An
entry that is given but cannot be converted |mdash| including a ``None``
inside ``argv`` or ``env`` |mdash| is ``PMIX_ERR_BAD_PARAM``, never
dropped. An app handed to a server's ``spawn`` handler has the same shape,
with ``cmd`` and ``cwd`` ``None`` where the library left them NULL.


DESCRIPTION
-----------

The `pmix_app_t` structure describes the application context for the
:ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>` and ``PMIx_Spawn_nb`` operations. Each
structure fully specifies one application to be launched.

The fields are:

``cmd``
   The executable to be launched, given as a string.

``argv``
   A ``NULL``-terminated, argv-style array of argument strings passed to the
   executable.

``env``
   A ``NULL``-terminated, argv-style array of environment variable strings to be
   set in the environment of the spawned processes.

``cwd``
   The current working directory in which the spawned processes are to be
   started.

``maxprocs``
   The maximum number of processes to be started with this application profile.

``info``
   An array of :ref:`pmix_info_t <man5-pmix_info_t>` structures containing
   attributes that describe this application (e.g., directives influencing how
   it is to be launched).

``ninfo``
   The number of elements in the ``info`` array.

STATIC INITIALIZER
------------------

A statically declared ``pmix_app_t`` may be initialized with the
``PMIX_APP_STATIC_INIT`` macro, which sets ``cmd``, ``argv``, ``env``, ``cwd``, and ``info`` to ``NULL`` and ``maxprocs`` and ``ninfo`` to ``0``:

.. code-block:: c

   pmix_app_t app = PMIX_APP_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
