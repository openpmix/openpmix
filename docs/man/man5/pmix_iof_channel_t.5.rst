.. _man5-pmix_iof_channel_t:

pmix_iof_channel_t
==================

.. include_body

`pmix_iof_channel_t` |mdash| Bit-mask identifying I/O forwarding channels

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint16_t pmix_iof_channel_t;

   #define PMIX_FWD_NO_CHANNELS        0x0000
   #define PMIX_FWD_STDIN_CHANNEL      0x0001
   #define PMIX_FWD_STDOUT_CHANNEL     0x0002
   #define PMIX_FWD_STDERR_CHANNEL     0x0004
   #define PMIX_FWD_STDDIAG_CHANNEL    0x0008
   #define PMIX_FWD_ALL_CHANNELS       0x00ff


DESCRIPTION
-----------

The `pmix_iof_channel_t` type is a 16-bit bit-mask that identifies one or more
input/output forwarding channels. The individual channel flags may be OR'd
together to reference multiple channels in a single value.

``PMIX_FWD_NO_CHANNELS`` (0x0000)
   No channels are selected.

``PMIX_FWD_STDIN_CHANNEL`` (0x0001)
   The standard input (``stdin``) channel.

``PMIX_FWD_STDOUT_CHANNEL`` (0x0002)
   The standard output (``stdout``) channel.

``PMIX_FWD_STDERR_CHANNEL`` (0x0004)
   The standard error (``stderr``) channel.

``PMIX_FWD_STDDIAG_CHANNEL`` (0x0008)
   The diagnostic channel, if one exists.

``PMIX_FWD_ALL_CHANNELS`` (0x00ff)
   All available channels.

The :ref:`PMIx_IOF_channel_string(3) <man3-PMIx_IOF_channel_string>` function
returns a string representation of a `pmix_iof_channel_t` value.


.. seealso::
   :ref:`PMIx_IOF_channel_string(3) <man3-PMIx_IOF_channel_string>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
