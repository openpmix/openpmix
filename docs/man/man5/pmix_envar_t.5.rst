.. _man5-pmix_envar_t:

pmix_envar_t
============

.. include_body

`pmix_envar_t` |mdash| Specifies a modification to an environment variable

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct {
       char *envar;
       char *value;
       char separator;
   } pmix_envar_t;


DESCRIPTION
-----------

The `pmix_envar_t` structure specifies a modification to be made to an
environment variable. Standard environment variables (e.g., ``PATH``,
``LD_LIBRARY_PATH``, and ``LD_PRELOAD``) take multiple arguments separated by
delimiters. Unfortunately, the delimiters depend upon the variable itself |mdash|
some use semi-colons, some colons, etc. Thus, the operation requires not only
the name of the variable to be modified and the value to be inserted, but also
the separator to be used when composing the aggregate value.

The fields are:

``envar``
   The name of the environment variable to be modified, given as a string.

``value``
   The value to be inserted into the named environment variable.

``separator``
   The single character used as the delimiter when composing the aggregate value
   of the environment variable from its constituent arguments.


.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
