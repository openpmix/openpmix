.. _man5-pmix_link_state_t:

pmix_link_state_t
=================

.. include_body

`pmix_link_state_t` |mdash| Describes the state of a fabric device port

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_link_state_t;

   #define PMIX_LINK_STATE_UNKNOWN     0
   #define PMIX_LINK_DOWN              1
   #define PMIX_LINK_UP                2


DESCRIPTION
-----------

The `pmix_link_state_t` type is an 8-bit unsigned integer describing the last
available physical port state of a fabric device. It takes one of the
following values:

``PMIX_LINK_STATE_UNKNOWN`` (0)
   The port state is unknown or not applicable.

``PMIX_LINK_DOWN`` (1)
   The port is inactive.

``PMIX_LINK_UP`` (2)
   The port is active.

A link state is conveyed, for example, via the ``PMIX_FABRIC_DEVICE_STATE``
attribute. The
:ref:`PMIx_Link_state_string(3) <man3-PMIx_Link_state_string>` function
returns a string representation of a `pmix_link_state_t` value.


.. seealso::
   :ref:`PMIx_Link_state_string(3) <man3-PMIx_Link_state_string>`,
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
