PROGRESS THREAD RESTRICTION
---------------------------

**A blocking PMIx call must not be made from within the PMIx progress
thread.** Any code the library itself invokes runs on that thread: an
event handler registered through
:ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
a callback passed to a non-blocking PMIx API, and |mdash| in a server or
tool |mdash| the completion of a host-module up-call. A blocking call
waits for work that the progress thread has to perform, so making one
from that thread waits for itself and never returns. The PMIx Standard
disallows it, and there is no way for an implementation to service such
a request.

Where this call has a blocking form |mdash| including the blocking
behavior a non-blocking entry point adopts when it is passed a ``NULL``
``cbfunc`` |mdash| that form detects the situation and returns
``PMIX_ERR_WOULD_BLOCK`` immediately, accompanied by a diagnostic naming
the call. Nothing is done and no callback is invoked.

``PMIX_ERR_WOULD_BLOCK`` here is not a transient condition to retry: it
reports a call that cannot be serviced from where it was made. Reissue
it as the non-blocking form with a callback, or from a thread of your
own.
