Resolving Peers and Nodes
=========================

PMIx provides two functions (``PMIx_Resolve_peers`` and ``PMIx_Resolve_nodes``) for discovering information about a given namespace. These are considered "convenience routines" as they are simple wrappers around basic PMIx APIs, designed to simplify access to commonly requested queries. However, providing that simplification results in a corresponding loss of clarity in the interpretation of any returned error code. Understanding the return status from these functions therefore requires a little knowledge of the underlying implementation.

The PMIx library is architected around the concept of organizing data by level - i.e., session-level information is collected in a corresponding session object, job-level information in a job object, etc. Both of the "resolve" functions address job-level information:

* ``PMIx_Resolve_peers`` - return the array of processes within the specified namespace that are executing on a given node.
* ``PMIx_Resolve_nodes`` - return a list of nodes hosting processes within the given namespace.

Knowledge regarding job-level layout is distributed across three layers:

* the host environment, which is considered the source of "absolute truth". The host is responsible for starting jobs and therefore must know where the processes for any given job are located. However, there is no requirement that every host daemon have complete knowledge of what is happening across the cluster, nor that it communicate that information to its PMIx server. Thus, it is left to the host to determine (a) what information it can provide, and (b) if it lacks information about a specified node or namespace, whether or not to generate a host-level query to obtain it. Note that the host will have up-to-the-moment information regarding changes to job layout - e.g., relocation or termination of a process due to faults. Such state changes may or may not have been communicated to the host's PMIx server (there is no requirement that the host do so, especially for jobs that have no local processes on that node).

* the PMIx server, which only has information about namespaces and nodes it was told about by the host. The PMIx server generally will not inform its clients about process distribution for namespaces other than the client's own. Thus, the server typically has a broader view of the situation than the client - but (as noted above) may not have as much information as its host.

* the PMIx client, which starts with information about its own job. Calls to ``PMIx_Connect`` and ``PMIx_Group_construct`` can expand that knowledge to include jobs that participate in those calls.

Given that hierarchy of knowledge, calls to the "resolve" APIs will attempt to retrieve the response from the highest available level:

1. If the client is *not* connected to a server, then the PMIx client library will provide an answer based on its own available (and somewhat limited) information. If the client *is* connected, then it will forward the request to the server.

2. When the server receives a request, or if the server generates its own "resolve" request, it first checks to see if its host supports the ``query`` upcall. If the host does support it, then the server will relay the request to the host via that interface. If the host does not support that interface, or if the host indicates it does not support that particular query, then the server will construct the response.

3. If the host does support the ``query`` upcall and the "resolve" request, then it constructs the response and returns it to the PMIx server for relay to the requesting client.

The status code returned by the APIs therefore depends somewhat on which level of the hierarchy generates the response. If the PMIx server or client is generating it, then the library follows some simple rules:

* Responding to either of these requests begins by finding the job object corresponding to the provided namespace. There is no parameter by which one can specify a session within which the job should be found, so the APIs are restricted to searching within the current session (remember, namespaces are only required to be unique within a session). If the namespace cannot be found, then the API will return the ``PMIX_ERR_INVALID_NAMESPACE`` status.

* If the namespace is found, then the search progresses to the node level. The PMIx library stores a list of nodes assigned to a given namespace on the corresponding job object. Only nodes assigned to that namespace are on the list. Thus, the response for ``PMIx_Resolve_nodes`` is constructed by simply collecting the hostnames from the nodes on the job object's list. In the event that the namespace has not currently been assigned any nodes, then ``PMIX_SUCCESS`` will be returned (to indicate that the request was successfully executed) and a ``NULL`` will be returned in the `nodelist` argument.

  Likewise, ``PMIx_Resolve_peers`` scans the list of nodes attached to the job object to find the specified node. If that node isn't found on the list, then the specified node is not currently assigned to the given namespace. In this case, the internal "fetch" operation will return a "not found" status to indicate that the node was not found on the list, and the ``PMIx_Resolve_peers`` function shall interpret this appropriately by returning ``PMIX_SUCCESS`` to the caller, with the `procs` array argument set to ``NULL`` and the `nprocs` argument set to zero.

  Note that it is possible that the specified node is not known to this PMIx server (e.g., the host didn't include it in any provided information, or the caller made a simple typo in the nspace parameter). There currently is no way for the library to return an error status indicating this situation. It will be treated as in the preceding paragraph.

* Once the node has been found on the list, the next step of the procedure is to scan the node-level values for that node to find the ``PMIX_LOCAL_PEERS`` attribute. This is the value the function is actually attempting to return to the caller. If the host provided it, then the function will return ``PMIX_SUCCESS`` and an array of the process IDs will be returned to the caller. Note that the host may have indicated that the node has been assigned to the namespace, but no processes are currently mapped to it. In this case, the ``PMIX_LOCAL_PEERS`` attribute will have a ``NULL`` value, and so the process ID array will be ``NULL``.

  If the host failed to provide the ``PMIX_LOCAL_PEERS`` attribute, then the function will return ``PMIX_ERR_DATA_VALUE_NOT_FOUND`` to indicate that the local peer information was not provided. It is possible that the PMIx library could reconstruct the local peers from other provided data. For example, the host may have provided a map of the individual process locations and not bothered with the specific ``PMIX_LOCAL_PEERS`` attribute for each node. While this might be an interesting extension to the current support, it is not presently available.

If the host is generating the response, it is solely responsible for determining the status code it will return. We strongly suggest that hosts follow the above logic to avoid confusion. The only requirement, however, is that the API return ``PMIX_SUCCESS`` if the API was successfully executed, even if no processes or no nodes were found.
