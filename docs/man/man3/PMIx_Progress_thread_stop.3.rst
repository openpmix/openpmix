.. _man3-PMIx_Progress_thread_stop:

PMIx_Progress_thread_stop
=========================

.. include_body

``PMIx_Progress_thread_stop`` |mdash| Halt the internal PMIx progress thread.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Progress_thread_stop(const pmix_info_t *info, size_t ninfo);


INPUT PARAMETERS
----------------

* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures used to
  tailor the operation (may be ``NULL``).
* ``ninfo``: Number of elements in the ``info`` array (may be ``0``).


DESCRIPTION
-----------

Stop the internal PMIx progress thread. By default the library runs a dedicated
thread that services events on its behalf; ``PMIx_Progress_thread_stop`` breaks
that thread's event loop and joins it, after which the library no longer makes
progress on its own. A host that has stopped the progress thread is responsible
for driving the library itself, for example via
:ref:`PMIx_Progress(3) <man3-PMIx_Progress>`.

The behavior of the operation may be adjusted through the ``info`` array. The
following attributes are recognized:

* ``PMIX_PROGRESS_THREAD_FLUSH`` (``bool``) |mdash| complete all pending events
  before stopping. This is the default behavior when the attribute is not
  supplied: a marker event is posted to the end of the thread's event list and
  the call blocks until that marker is reached, ensuring that events already
  queued are processed before the thread is halted.
* ``PMIX_PROGRESS_THREAD_NAME`` (``char*``) |mdash| the name of a specific
  progress thread to stop. If this attribute is not provided, the library's
  shared (default) progress thread is stopped.

If the targeted progress thread is not currently active, the call has no effect.


RETURN VALUE
------------

``PMIx_Progress_thread_stop`` returns no value (``void``).


NOTES
-----

``PMIx_Progress_thread_stop`` is a PMIx library extension: it is not defined in
the PMIx Standard. Its signature and behavior are governed by the OpenPMIx
implementation.

Once the internal progress thread has been stopped, PMIx APIs that rely on the
progress engine will no longer be serviced automatically. The host must either
restart progress or drive it explicitly (see
:ref:`PMIx_Progress(3) <man3-PMIx_Progress>`) to keep operations moving.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Progress(3) <man3-PMIx_Progress>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
