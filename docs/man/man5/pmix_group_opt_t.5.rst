.. _man5-pmix_group_opt_t:

pmix_group_opt_t
================

.. include_body

`pmix_group_opt_t` |mdash| Response to a PMIx group invitation

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef enum {
       PMIX_GROUP_DECLINE,
       PMIX_GROUP_ACCEPT
   } pmix_group_opt_t;

Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   opt = PMIX_GROUP_ACCEPT

where ``opt`` is one of the ``pmix_group_opt_t`` enumerated values.


DESCRIPTION
-----------

The `pmix_group_opt_t` enumeration defines the values used to indicate
acceptance or declination of an invitation to join a PMIx group. The type
exists primarily for the readability of user code responding to a group
invitation. The defined values are:

* ``PMIX_GROUP_DECLINE`` (value 0) |mdash| decline the invitation.

* ``PMIX_GROUP_ACCEPT`` (value 1) |mdash| accept the invitation.


.. seealso::
   :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`,
   :ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>`
