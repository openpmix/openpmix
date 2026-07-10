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


.. seealso::
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
