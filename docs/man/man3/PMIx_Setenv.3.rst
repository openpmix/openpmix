.. _man3-PMIx_Setenv:

PMIx_Setenv
===========

.. include_body

``PMIx_Setenv`` |mdash| Set an environment variable in a caller-provided
environment array


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Setenv(const char *name,
                             const char *value,
                             bool overwrite,
                             char ***env);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``name``: Name of the environment variable to set.
* ``value``: Value to assign to the variable. If ``NULL``, the variable is
  either removed (when ``env`` refers to the process ``environ``) or set to
  an empty value.
* ``overwrite``: If ``true``, replace any existing entry for ``name``; if
  ``false``, leave an existing entry unchanged.


INPUT/OUTPUT PARAMETERS
-----------------------

* ``env``: Address of an ``argv``-style, ``NULL``-terminated array of
  ``"name=value"`` strings to be updated. Must not be ``NULL``. The pointed-to
  array may be grown (reallocated) to accommodate a new entry.


DESCRIPTION
-----------

Set an environment variable within a caller-supplied environment array, using
``argv``-style ``"name=value"`` string conventions.

The behavior depends on what ``env`` references:

* If ``*env`` is the process ``environ``, the operation is delegated to the C
  library: a ``NULL`` ``value`` performs an ``unsetenv(name)``, otherwise
  ``setenv(name, value, overwrite)`` is called.
* Otherwise the routine operates on the caller's array. A new
  ``"name=value"`` string (or ``"name="`` when ``value`` is ``NULL``) is
  formatted. If ``*env`` is ``NULL`` or contains no entry for ``name``, the
  new string is appended, growing the array. If an entry for ``name`` already
  exists, it is replaced when ``overwrite`` is ``true`` and left untouched
  (returning ``PMIX_ERR_EXISTS``) when ``overwrite`` is ``false``.

Memory for the array entries is managed by the library; strings that are
replaced are freed and the array is reallocated as needed.


RETURN VALUE
------------

Returns one of the following ``pmix_status_t`` values:

* ``PMIX_SUCCESS``: The variable was set (or unset), or already existed and
  ``overwrite`` was ``false`` for the ``environ`` fast path.
* ``PMIX_ERR_BAD_PARAM``: The ``env`` argument was ``NULL``.
* ``PMIX_ERR_EXISTS``: An entry for ``name`` already existed in a
  caller-provided array and ``overwrite`` was ``false``.
* ``PMIX_ERR_OUT_OF_RESOURCE``: Memory for the new entry could not be
  allocated.


NOTES
-----

``PMIx_Setenv`` is an OpenPMIx convenience routine. When ``*env`` points at
the global ``environ``, the update follows C-library ``setenv``/``unsetenv``
semantics; for any other array the entry is stored using PMIx ``argv``-style
management. The array remains ``NULL``-terminated after the call.
