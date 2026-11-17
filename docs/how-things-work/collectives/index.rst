Server-Side Collective Tracking
===============================

PMIx collective operations (``PMIx_Fence``, ``PMIx_Connect``,
``PMIx_Disconnect``, and group construct/destruct) are assembled in two
phases: the server first gathers the contributions of all *local*
participants, then hands the assembled operation up to its host
environment for *global* completion across nodes. This section describes
how the server tracks local participation, why the historical
counter-based accounting is unsafe when a participant is lost, and the
identity-based tracking model that replaces it, and what the server
then tells the host about who took part.

.. toctree::
   :maxdepth: 2

   tracking_spec
   tracking_plan
   local_participants
