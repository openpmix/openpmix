.. _man3-PMIx_Publish:

PMIx_Publish
============

.. include_body

``PMIx_Publish``, ``PMIx_Publish_nb`` |mdash| Publish data for later access by
other processes via :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Publish(const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Publish_nb(const pmix_info_t info[], size_t ninfo,
                                 pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the data to publish is a list of Python ``pmix_info_t`` dictionaries;
  # each entry carries the key/value to publish, plus any directives
  pydata = [{'key': "mykey",
             'value': {'value': "myvalue", 'val_type': PMIX_STRING}},
            {'key': PMIX_RANGE,
             'value': {'value': PMIX_RANGE_SESSION, 'val_type': PMIX_UINT8}}]
  rc = foo.publish(pydata)


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures. Each entry conveys either a key/value pair to be published or a
  directive that qualifies the operation (see `DIRECTIVES`_). A ``NULL`` value is
  not permitted |mdash| there must be at least one item to publish.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form adds a callback:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked once the
  datastore confirms that the data has been posted.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Publish the data in the ``info`` array for subsequent lookup. Each entry in the
array that is not a recognized directive is treated as a key/value pair to be
made available to other processes. Once published, the data can be retrieved by
any process that meets the access constraints by referring solely to its key
via :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>` |mdash| neither the publisher nor
the eventual recipient need know the identity of the other in advance.

By default, the data is published into the ``PMIX_RANGE_SESSION`` range with
``PMIX_PERSIST_APP`` persistence. Those defaults, and any additional directives,
may be changed by including the appropriate attributes in the ``info`` array.
Attempts to access the data by processes that fall outside the specified range
are rejected. The ``PMIX_PERSISTENCE`` attribute instructs the datastore how
long the information is to be retained.

Keys must be unique within the specified data range |mdash| the first published
value wins. Publishing a duplicate key on the *same* range returns
``PMIX_ERR_DUPLICATE_KEY``; publishing the same key on *different* ranges is
permitted.

``PMIx_Publish`` is the blocking form: it does not return until the datastore
has confirmed that the data is available for lookup. The ``info`` array may be
released as soon as the blocking call returns.

``PMIx_Publish_nb`` is the non-blocking form: it returns immediately and invokes
``cbfunc`` when the datastore confirms availability of the data. As with all
non-blocking PMIx APIs, the caller **must** keep the ``info`` array valid until
``cbfunc`` is invoked.

The PMIx library automatically adds the publishing process's ``PMIX_USERID`` and
``PMIX_GRPID`` to the information passed to the host environment; published data
is constrained to access by that user ID.


DIRECTIVES
----------

The following attributes qualify this operation. PMIx libraries are not required
to support any of them directly, but must pass any that are provided to the host
environment; where supported, they are optional for host environments.

* ``PMIX_RANGE`` (pmix_data_range_t) |mdash| define the constraint on which
  processes may access the published data (e.g., ``PMIX_RANGE_SESSION``,
  ``PMIX_RANGE_NAMESPACE``, ``PMIX_RANGE_GLOBAL``). Only processes that meet the
  constraint are allowed to access it. Defaults to ``PMIX_RANGE_SESSION``.
* ``PMIX_PERSISTENCE`` (pmix_persistence_t) |mdash| declare how long the
  datastore is to retain the data (e.g., ``PMIX_PERSIST_FIRST_READ``,
  ``PMIX_PERSIST_PROC``, ``PMIX_PERSIST_APP``, ``PMIX_PERSIST_SESSION``,
  ``PMIX_PERSIST_INDEF``). Defaults to ``PMIX_PERSIST_APP``.
* ``PMIX_ACCESS_PERMISSIONS`` (pmix_data_array_t) |mdash| define access
  permissions for the published data. The value contains an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures specifying the permissions.
* ``PMIX_ACCESS_USERIDS`` (pmix_data_array_t) |mdash| array of effective user IDs
  that are allowed to access the published data.
* ``PMIX_ACCESS_GRPIDS`` (pmix_data_array_t) |mdash| array of effective group IDs
  that are allowed to access the published data.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the operation to
  complete before returning an error (0 => infinite).


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the data has been posted
and is available for lookup. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing; the
final status is delivered to ``cbfunc``. A non-blocking call may instead return
``PMIX_OPERATION_SUCCEEDED`` to indicate that the request was immediately
processed and succeeded, in which case ``cbfunc`` will **not** be called.

* ``PMIX_SUCCESS`` |mdash| the data has been published.
* ``PMIX_ERR_DUPLICATE_KEY`` |mdash| a provided key has already been published on
  the same data range.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, a ``NULL`` ``info`` array.
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Because published data is constrained by both range and user ID,
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>` will only return data published by the
same user whose range includes the requesting process. Choose the publication
range deliberately so the intended recipients fall within it.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
