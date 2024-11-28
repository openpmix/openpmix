Group Construction
==================

For purposes of constructing PMIx groups, PMIx defines two
classes of group members:

*leaders* have some global view of the group at time of
construction. This might consist of knowing the number of
leaders in the group, or knowing the process IDs of all
group leaders.

*members* know only that they are to participate in a given
group ID, but have no other knowledge of the group. For example,
a member may not know how many processes will be in the group
or any of their process IDs. The only requirement for membership
is that the process know the group ID to which they are to belong.

Within that context,
PMIx supports three methods for constructing PMIx groups. The
*collective* method is considered the more traditional form
of the operation but requires all group leaders to know the process ID
of all other leaders prior to calling the API.

In contrast,
the *bootstrap* method is a somewhat more dynamic form of the operation
that assumes each leader only knows the number of group leaders,
but does not know their process IDs.

In either of these two methods, additional group members can be
specified by any leader via the ``PMIX_GROUP_ADD_MEMBERS``
attribute. The PMIx server library and host are jointly responsible
for aggregating the
additional group members specified across leaders. Processes that are on the
"additional member" list must call ``PMIx_Group_construct``
with ``NULL`` in the  ``procs`` argument - this
indicates that the process is to
be added to the group when the group construct operation has completed.

Note that the group construct operation _cannot_ complete until all
"add members" have
called ``PMIx_Group_construct``. This is required so that any group
and/or endpoint information
provided by the added members can be included in the returned
``pmix_info_t`` array.


Finally, the *invite* method represents the most dynamic form
of group construction as it is executed in an ad hoc manner that
revolves around a single leader that asynchronously invites
processes to join a group.

Each of these methods is explained further below.


Collective Method
-----------------

All leaders know the ID of all other leaders, and thus call
``PMIx_Group_construct`` with the array of all leader process IDs.
Note that in this method, _all_ leaders _must_ call the API
with the array of process IDs.


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
Bootstrap is used when the processes leading group construct do
not know the identity of all other processes that will be participating, but at least
know how may leaders will be participating. Leaders provide only their
own process ID in the ``procs`` parameter to the ``PMIx_Group_construct``
API, and are _required_ to include the
``PMIX_GROUP_BOOTSTRAP`` attribute in their array of ``pmix_info_t``
directives, with the value in that attribute set to equal the number
of leaders in the group construct operation.



