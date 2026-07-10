.. _man3-PMIx_Envar_load:

PMIx_Envar_load
===============

.. include_body

``PMIx_Envar_load`` |mdash| Populate a
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structure with an environment
variable name, value, and separator.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Envar_load(pmix_envar_t *e,
                        char *var,
                        char *value,
                        char separator);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to a previously constructed :ref:`pmix_envar_t(5)
  <man5-pmix_envar_t>` structure to be populated.
* ``var``: Name of the environment variable. If non-``NULL``, a copy of the
  string is stored in the ``envar`` field.
* ``value``: Value to associate with the environment variable. If non-``NULL``,
  a copy of the string is stored in the ``value`` field.
* ``separator``: Character used to separate multiple values when composing an
  aggregate value for the variable (for example, ``':'`` for ``PATH``-style
  variables). Stored in the ``separator`` field.


DESCRIPTION
-----------

Populate the fields of a :ref:`pmix_envar_t <man5-pmix_envar_t>` structure. The
``var`` and ``value`` strings are each **duplicated** into newly allocated
storage owned by the structure; the caller retains ownership of the strings it
passed in. If ``var`` (or ``value``) is ``NULL``, the corresponding field is
left unchanged. The ``separator`` character is copied directly.

The target structure should have been initialized with
:ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>` (or created with
:ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`) before loading, so that
its pointer fields hold known values.


RETURN VALUE
------------

``PMIx_Envar_load`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Envar_load`` copies the supplied strings, the memory it allocates
must eventually be released with :ref:`PMIx_Envar_destruct(3)
<man3-PMIx_Envar_destruct>` (for a single caller-owned structure) or
:ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>` (for an array obtained from
:ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`).


EXAMPLES
--------

Load a colon-separated ``PATH`` addition:

.. code-block:: c

    pmix_envar_t envar;

    PMIx_Envar_construct(&envar);
    PMIx_Envar_load(&envar, "PATH", "/opt/bin", ':');

    /* ... use the structure ... */

    PMIx_Envar_destruct(&envar);


.. seealso::
   :ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>`,
   :ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>`,
   :ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`,
   :ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>`,
   :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
