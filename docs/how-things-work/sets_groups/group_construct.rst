Group Construction
==================

PMIx supports two methods for constructing PMIx groups. The
*collective* method is considered the more traditional form
of the operation but requires all members of the group
invoking the ``PMIx_Group_construct`` to know the proc ID
of all other members prior to calling the API.

In contrast,
the *bootstrap* method is a more dynamic form of the operation
that assumes only a core subset of participants know of each other
prior to calling the API - but that some or all of those
participants know of _additional_ processes that need to be
included in the final group. Bootstrap also requires that
those additional processes know (a) that they need to join
the group, and (b) the group ID of the group they need
to join.

Each of these methods is explained further below.


Collective Method
-----------------

All participants know the ID of all other participants, and thus call
``PMIx_Group_construct`` with the array of all participating proc IDs.
Note that in this method, _all_ participants _must_ call the API
with the array of participating proc IDs. No participants can be
added using the ``PMIX_GROUP_ADD_MEMBERS`` attribute.


Host responsibilities
^^^^^^^^^^^^^^^^^^^^^

Perform collective allgather operation across participants, returning
all job information (if different namespaces are involved) and all
information posted by participating individual procs via ``PMIx_Put``.
Note that this therefore requires that participants within a namespace
complete a ``PMIx_Commit``/``PMIx_Fence`` operation prior to being involved in a
``PMIx_Group_construct`` operation to ensure that the host environment
has access to posted information.


Bootstrap Method
----------------
Bootstrap is used when the processes involved in group construct do
not know the identity of all other processes that will be participating.
It is required, however, that all participants at least know how many
processes will be participating.

In this context, participants equate to processes that call ``PMIx_Group_construct`` (or
its non-blocking equivalent) and pass _only_ their own process identifier
to the ``procs`` argument. Participants are _required_ to include the
``PMIX_GROUP_BOOTSTRAP`` attribute in their array of ``pmix_info_t``
directives, with the value in that attribute set to equal the number
of participants in the group construct operation.


Add Members
-----------
Additional group members can be specified by any participant via the ``PMIX_GROUP_ADD_MEMBERS``
attribute. The PMIx server library and host are jointly responsible for aggregating the
additional group members specified across participants. Processes that are on the
"additional member" list must call ``PMIx_Group_construct``
with a ``NULL`` ``procs`` argument - this indicates that the process is to
be added to the group (via PMIx event, internal to the ``PMIx_Group_construct`` function)
when the group construct operation has completed.

Participant in this context equates to any process that calls ``PMIx_Group_construct``,
whether bootstrapping or not.

Note that the group construct operation _cannot_ complete until all "add members" have
called ``PMIx_Group_construct``. This is required so that any group and/or endpoint information
provided by the added members can be included in the returned ``pmix_info_t`` array.
