.. _man3-PMIx_Lookup:

PMIx_Lookup
===========

.. include_body

``PMIx_Lookup``, ``PMIx_Lookup_nb`` |mdash| Retrieve data published by this or
another process via :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Lookup(pmix_pdata_t data[], size_t ndata,
                             const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Lookup_nb(char **keys, const pmix_info_t info[], size_t ninfo,
                                pmix_lookup_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # data is a list of Python ``pmix_pdata_t`` dictionaries; only the
  # ``key`` field is used on input to name the values to retrieve
  data = [{'key': "mykey", 'value': {'value': None, 'val_type': PMIX_UNDEF},
           'proc': {'nspace': "", 'rank': 0}}]
  # directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_RANGE,
             'value': {'value': PMIX_RANGE_SESSION, 'val_type': PMIX_UINT8}}]
  rc, data = foo.lookup(data, pydirs)


INPUT PARAMETERS
----------------

Blocking form:

* ``data``: Pointer to an array of ``pmix_pdata_t`` structures. On input, the
  ``key`` field of each element names a value to retrieve; the array must not be
  ``NULL``. See `OUTPUT PARAMETERS`_ for how results are returned.
* ``ndata``: Number of elements in the ``data`` array.

Non-blocking form:

* ``keys``: A ``NULL``-terminated array of key strings naming the values to
  retrieve. Must not be ``NULL``.

Both forms:

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``data`` (blocking form): For each element whose key was found, the ``value``
  field is populated with the published value and the ``proc`` field is set to
  the namespace/rank of the process that published it. Any key that cannot be
  found is left with a value of type ``PMIX_UNDEF``. The caller must therefore
  check each element to determine which were returned.

The non-blocking form replaces ``data`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_lookup_cbfunc_t`` invoked with an
  array of ``pmix_pdata_t`` structures for the found keys once the lookup
  completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Look up information previously published by this or another process with
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>`. The lookup is resolved solely by
key |mdash| the requester need not know the identity of the publisher.

By default, the search is constrained to publishers within the
``PMIX_RANGE_SESSION`` range (which disambiguates duplicate keys published on
different ranges). The range may be changed |mdash| for example, expanded to all
potential publishers via ``PMIX_RANGE_GLOBAL`` |mdash| and other directives
added through the ``info`` array. The search is additionally constrained to data
published by the current user ID: it will not return data published by an
application running under a different user. There is currently no option to
override that behavior.

``PMIx_Lookup`` is the blocking form. It returns ``PMIX_SUCCESS`` if *any* of the
requested keys are found, so the caller must inspect each ``data`` element to
learn which values were actually returned. ``PMIx_Lookup_nb`` is the non-blocking
form: it returns immediately and delivers the found data to ``cbfunc``. As with
all non-blocking PMIx APIs, the caller **must** keep the ``keys`` and ``info``
arrays valid until ``cbfunc`` is invoked.

By default, a lookup does **not** wait for the requested data to be published;
it blocks only for the time the datastore needs to search its current contents
and return any matches. The caller is therefore responsible for ensuring that
the data is published before the lookup, for using the ``PMIX_WAIT`` directive to
instruct the datastore to wait until the data becomes available, or for retrying
until the requested data is found.


DIRECTIVES
----------

The following attributes qualify this operation. PMIx libraries are not required
to support any of them directly, but must pass any that are provided to the host
environment; where supported, they are optional for host environments.

* ``PMIX_RANGE`` (pmix_data_range_t) |mdash| restrict the search to publishers
  within the specified range (e.g., ``PMIX_RANGE_SESSION``,
  ``PMIX_RANGE_NAMESPACE``, ``PMIX_RANGE_GLOBAL``). Defaults to
  ``PMIX_RANGE_SESSION``.
* ``PMIX_WAIT`` (int) |mdash| direct the datastore to wait until the requested
  data has been published rather than returning immediately with whatever is
  currently available. The datastore waits until all requested data is available.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, to wait for the data
  to become available before returning an error (0 => infinite). Helps avoid
  indefinite waits when combined with ``PMIX_WAIT``.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that at least one requested key
was found and returned. For the non-blocking form, a return of ``PMIX_SUCCESS``
indicates only that the request was accepted for processing; the final status and
data are delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| all (blocking form: at least one) requested keys were
  found and returned.
* ``PMIX_ERR_PARTIAL_SUCCESS`` |mdash| only some of the requested data was found.
  In the non-blocking form only the found data is included in the returned array;
  the specific reason a particular item is missing (e.g., lack of permissions)
  cannot be communicated back.
* ``PMIX_ERR_NOT_FOUND`` |mdash| none of the requested data could be found within
  the requester's range.
* ``PMIX_ERR_NO_PERMISSIONS`` |mdash| all requested data was found and the range
  restrictions were met, but none could be returned due to lack of access
  permissions.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| no datastore on this system supports this
  function.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, a ``NULL`` ``data`` array (blocking) or ``NULL`` ``keys`` array
  (non-blocking).
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Because a successful blocking return does not guarantee that every key was found,
always examine each returned ``pmix_pdata_t`` element: an element left with a
value type of ``PMIX_UNDEF`` was not resolved. Data is only visible to a
requester whose user ID matches the publisher and whose process falls within the
publication range.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
