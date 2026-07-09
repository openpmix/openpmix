.. _man3-PMIx_Progress:

PMIx_Progress
=============

.. include_body

``PMIx_Progress`` |mdash| Step the PMIx progress engine when the host is
driving progress of the library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Progress(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  foo.progress()


DESCRIPTION
-----------

Advance the PMIx library's internal progress engine by one iteration. Normally
the library runs a dedicated internal progress thread that services events on
its own. When that thread is disabled |mdash| for example, when the host has
requested to drive progress itself by passing the ``PMIX_EXTERNAL_PROGRESS``
attribute to :ref:`PMIx_Init(3) <man3-PMIx_Init>` |mdash| no such thread runs,
and the host becomes responsible for periodically stepping the engine.

``PMIx_Progress`` performs a single, non-blocking pass over the library's event
base: it dispatches any events that are currently ready and then returns
immediately, without waiting for further activity. The host is expected to call
it repeatedly (for example, from within its own progress or event loop) so that
PMIx operations continue to make forward progress.

``PMIx_Progress`` takes no arguments. If the library has no shared progress
engine configured, the call is a no-op and returns without doing anything.


RETURN VALUE
------------

``PMIx_Progress`` returns no value (``void``).


NOTES
-----

Special care must be taken to avoid deadlocking when driving progress from
within PMIx callback functions and APIs, as reentering the library from a
context that is itself being progressed can produce circular waits.

``PMIx_Progress`` is only meaningful when the internal progress thread has been
disabled. When PMIx is running its own progress thread, calling this function is
unnecessary.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Progress_thread_stop(3) <man3-PMIx_Progress_thread_stop>`
