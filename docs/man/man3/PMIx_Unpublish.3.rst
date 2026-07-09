.. _man3-PMIx_Unpublish:

PMIx_Unpublish
==============

.. include_body

``PMIx_Unpublish``, ``PMIx_Unpublish_nb`` |mdash| Remove data previously posted
via :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Unpublish(char **keys,
                                const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Unpublish_nb(char **keys,
                                   const pmix_info_t info[], size_t ninfo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # pykeys is a list of key strings to remove (None removes all)
  pykeys = ["mykey"]
  # directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_RANGE,
             'value': {'value': PMIX_RANGE_SESSION, 'val_type': PMIX_UINT8}}]
  rc = foo.unpublish(pykeys, pydirs)


INPUT PARAMETERS
----------------

* ``keys``: A ``NULL``-terminated array of key strings identifying the published
  data to remove. A ``NULL`` value instructs the server to remove **all** data
  published by the calling process.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form adds a callback:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked once the
  server confirms removal of the specified data.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Remove data previously posted by this process with
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>`, using the provided ``keys``. A
``NULL`` ``keys`` array removes all data published by the calling process within
the specified range.

By default, the range is assumed to be ``PMIX_RANGE_SESSION``. That default, and
any additional directives, may be changed by including the appropriate attributes
in the ``info`` array. The range supplied here must match the range under which
the data was published.

``PMIx_Unpublish`` is the blocking form: it does not return until the server has
confirmed removal of the data |mdash| after which it is safe to publish the same
key again within the specified range. ``PMIx_Unpublish_nb`` is the non-blocking
form: it returns immediately and invokes ``cbfunc`` once the server confirms
removal. As with all non-blocking PMIx APIs, the caller **must** keep the
``keys`` and ``info`` arrays valid until ``cbfunc`` is invoked.

The PMIx library automatically adds the requesting process's ``PMIX_USERID`` and
``PMIX_GRPID`` to the information passed to the host environment; a process can
only unpublish data that it published.


DIRECTIVES
----------

The following attributes qualify this operation. PMIx libraries are not required
to support any of them directly, but must pass any that are provided to the host
environment; where supported, they are optional for host environments.

* ``PMIX_RANGE`` (pmix_data_range_t) |mdash| the range from which the data is to
  be removed (e.g., ``PMIX_RANGE_SESSION``, ``PMIX_RANGE_NAMESPACE``,
  ``PMIX_RANGE_GLOBAL``). Must match the range under which the data was
  published. Defaults to ``PMIX_RANGE_SESSION``.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the operation to
  complete before returning an error (0 => infinite).


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the specified data has
been removed. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates
only that the request was accepted for processing; the final status is delivered
to ``cbfunc``. A non-blocking call may instead return
``PMIX_OPERATION_SUCCEEDED`` to indicate that the request was immediately
processed and succeeded, in which case ``cbfunc`` will **not** be called.

* ``PMIX_SUCCESS`` |mdash| the specified data has been removed.
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Unpublishing is scoped to the publishing process and to the specified range: a
process cannot remove data published by another process, and the range supplied
must match the one used at publication time. After a successful unpublish, the
same key may be published again within that range.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
