Process Sets
============

PMIx supports two slightly related, but functionally different concepts
known as *process sets* and *process groups*. This section defines
these two concepts and describes how they are utilized, along with their
corresponding APIs.


Process Set Attributes
----------------------

Several attributes are provided for querying the system regarding process sets using the
PMIx_Query_info API. These include the following:

* ``PMIX_QUERY_NUM_PSETS``
  * String: ``pmix.qry.psetnum``
  * Type: size_t
  * Description: Return the number of process sets defined in the specified range (defaults to ``PMIX_RANGE_SESSION``).

* ``PMIX_QUERY_PSET_NAMES``
  * String: ``pmix.qry.psets``
  * Type: pmix_data_array_t*
  * Description: Return a ``pmix_data_array_t`` containing an array of strings of the process set names defined in the specified range (defaults to ``PMIX_RANGE_SESSION``).

* ``PMIX_QUERY_PSET_MEMBERSHIP``
  * String: ``pmix.qry.pmems``
  * Type: pmix_data_array_t*
  * Description: Return an array of ``pmix_proc_t`` containing the members of the specified process set.

In addition, the ``PMIX_PROCESS_SET_DEFINE`` event shall include the name of the newly defined
process set and its members using the following attributes:

* ``PMIX_PSET_NAME``
  * String: ``pmix.pset.nm``
  * Type: char*
  * Description: The name of the newly defined process set.

* ``PMIX_PSET_MEMBERS``
  * String: ``pmix.pset.mems``
  * Type: pmix_data_array_t*
  * Description: An array of ``pmix_proc_t`` containing the members of the newly defined process set.

Finally, a process can request (via ``PMIx_Get``) the process sets to which a given process (including itself)
belongs using the following attribute:

* ``PMIX_PSET_NAMES``
  * String: ``pmix.pset.nms``
  * Type: pmix_data_array_t*
  * Description: Returns an array of ``char*`` string names of the process sets in which the given process is a member.

