Overview
========

Describe general layout for operation. Tools and apps can potentially
communicate directly to the scheduler, passing requests and getting
responses. Or, more commonly, they can communicate such requests to
their local runtime environment (RTE), which can then act as a relay
between the requestor and the scheduler.

In addition, the RTE itself can use its connection to the scheduler
for coordinating their interactions. For example, the scheduler may
use PMIx to inform the RTE of a new session it should instantiate.
Likewise, the RTE can use PMIx to inform the scheduler that a session
has terminated and thus all its resources are available for reuse.

While the PMIx scheduler integration support can be used for the
more common operations (e.g., submitting allocation requests,
RTE-scheduler coordination), the primary intent of the support is
to enable dynamic environments - i.e., the ability for an application
to request on-the-fly modifications of its session. 

Support is provided via a few APIs, attributes, and events.

