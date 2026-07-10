.. _man3-PMIx_Register_attributes:

PMIx_Register_attributes
========================

.. include_body

``PMIx_Register_attributes`` |mdash| Register the attributes the host
environment supports for a given server-module function.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_Register_attributes(char *function, char *attrs[]);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # attrs is a list of attribute-name strings supported for the function
  attrs = ["PMIX_TIMEOUT", "PMIX_WAIT"]
  rc = foo.register_attributes("allocate", attrs)


INPUT PARAMETERS
----------------

* ``function``: The string name of the ``pmix_server_module_t`` function whose
  supported attributes are being registered |mdash| for example,
  ``"register_events"``, ``"validate_credential"``, or ``"allocate"``.
* ``attrs``: A ``NULL``-terminated ``argv``-style array of attribute-name strings
  supported by the host environment for the specified ``function``.


DESCRIPTION
-----------

Register with the local PMIx server library the attributes that the host
environment supports for a specific ``pmix_server_module_t`` function. Host
environments use this API so that, when a client or tool later queries the
library (via :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>` with the
``PMIX_QUERY_ATTRIBUTE_SUPPORT`` key) for the attributes supported by a
user-facing API, the library can accurately report which attributes the host will
honor.

Attributes registered through this API are recorded at the **host** level
(``PMIX_HOST_ATTRIBUTES``); the PMIx library separately and automatically
registers the attributes it supports internally at the client, server, and tool
levels during initialization. It is the library's responsibility to associate the
host-registered attributes for a server-module function with their corresponding
user-facing API and level when answering a query.

This is a **blocking** operation: internally the request is thread-shifted onto
the progress thread, and the call does not return until the registration has been
recorded. The ``function`` name and ``attrs`` array are copied by the library, so
the caller need not keep them valid after the call returns.

Host environments are strongly encouraged to register all supported attributes
immediately after initializing the library (see
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`), before servicing any user
requests, so that attribute-support queries are correctly answered from the
outset.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the attributes are successfully registered.
Otherwise a negative PMIx error constant is returned, including:

* ``PMIX_ERR_REPEAT_ATTR_REGISTRATION`` |mdash| the attributes for this function
  have already been registered at the host level. Duplicate registration at the
  same level for a function is not permitted; registrations at different levels
  are tracked independently.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid registration level was specified
  internally.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This is a server-role API, available only after
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`.

The signature shown above is that of the OpenPMIx library. The PMIx Standard
describes an alternative signature for this function that takes an array of
``pmix_regattr_t`` structures together with a count; the implementation in this
release accepts the simpler ``NULL``-terminated string array shown here.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
