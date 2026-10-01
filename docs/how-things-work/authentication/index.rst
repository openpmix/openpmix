Authentication
==============

Before a PMIx server answers a request, it needs to know who is asking.
For a connection between processes, "who" usually means the user ID and
group ID of the process on the other end of the socket. Those IDs decide
which jobs, data, and output the peer may see. This section covers how
those IDs can be established, and how far each method can be trusted.

.. toctree::
   :maxdepth: 2

   methods
   native
