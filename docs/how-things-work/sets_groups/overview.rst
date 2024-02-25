Overview
========

PMIx supports two slightly related, but functionally different concepts
known as *process sets* and *process groups*. This section defines
these two concepts and describes how they are utilized, along with their
corresponding APIs.


Process Sets vs. Groups
-----------------------

A PMIx *Process Set* is a user-provided or host environment assigned
label associated with a given set of application processes. Processes can
belong to multiple process sets at a time. Users may define a PMIx
process set at time of application execution. For example, if using the
command line parallel launcher ``prun``, one could specify process sets
as follows:

.. code::

	$ prun -n 4 --pset ocean myoceanapp : -n 3 --pset ice myiceapp

In this example, the processes in the first application will be labeled with a ``PMIX_PSET_NAME``
attribute with a value of *ocean* while those in the second application will be labeled with an
*ice* value. During the execution, application processes could lookup the process set attribute
for any process using ``PMIx_Get``. Alternatively, other executing applications could utilize the
``PMIx_Query_info`` APIs to obtain the number of declared process sets in the system, a list of
their names, and other information about them. In other words, the *process set* identifier provides
a label by which an application can derive information about a process and its application - it does
*not*, however, confer any operational function.

Host environments can create or delete process sets at any time through the
``PMIx_server_define_process_set`` and
``PMIx_server_delete_process_set`` APIs. PMIx servers shall
notify all local clients of process set operations via the
``PMIX_PROCESS_SET_DEFINE`` or ``PMIX_PROCESS_SET_DELETE``
events.

Process *sets* differ from process *groups* in several key ways:

* Process *sets* have no implied relationship between their members - i.e., a process in a process set has no concept of a ``pset rank`` as it would in a process *group*.

    * Process *set* identifiers are set by the host environment or by the user at time of application submission for execution -
      there are no PMIx client API's provided by which an application can define a process set or change a process set membership.
      In contrast, PMIx process *groups* can only be defined dynamically by the application.

    * Process *sets* are immutable - members cannot be added or removed once the set has been defined. In contrast, PMIx process *groups* can dynamically
      change their membership using the appropriate API's.

    * Process *groups* can be used in calls to PMIx operations. Members of process *groups* that are involved in an operation are translated by their
      PMIx library into their *native* identifier prior to the operation being passed to the host environment. For example, an application can define a
      process group to consist of ranks 0 and 1 from the host-assigned namespace of *210456*, identified by the group id of *foo*. If the application subsequently calls the PMIx_Fence API with a process identifier of \{foo, PMIX_RANK_WILDCARD\}, PMIx library will replace that identifier with an array
      consisting of \{210456, 0\} and \{210456, 1\} - the host-assigned identifiers of the participating processes - prior to processing the request.

    * Process *groups* can request that the host environment assign a unique size_t *context identifier* to the group at time of group construction.
      An MPI library may, for example, use the ID as the MPI communicator identifier for the group.

The two concepts do, however, overlap in that they both
involve collections of processes. Users desiring to create a process group
based on a process set could, for example, obtain the membership array of the
process set and use that as input to PMIx_Group_construct, perhaps
including the process set name as the group identifier for clarity. Note that
no linkage between the set and group of the same name is implied nor
maintained - e.g., changes in process group membership can not be
reflected in the process set using the same identifier.

.. note::

    The host environment is responsible for ensuring:

    * consistent knowledge of process set membership across all involved PMIx servers; and

    * that process set names do not conflict with system-assigned namespaces within the scope of the set.

