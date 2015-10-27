---
layout: page
title: PMIx_Commit(3)
tagline: PMIx Programmer's Manual
---
{% include JB/setup %}

# NAME

PMIx_Fence[_nb] - Execute a blocking[non-blocking] barrier across the processes identified in the
specified array.

# SYNOPSIS

{% highlight c %}
#include <pmix.h>

pmix_status_t PMIx_Fence(const pmix_proc_t procs[], size_t nprocs,
                         const pmix_info_t info[], size_t ninfo);

pmix_status_t PMIx_Fence_nb(const pmix_proc_t procs[], size_t nprocs,
                            const pmix_info_t info[], size_t ninfo,
                            pmix_op_cbfunc_t cbfunc, void *cbdata);


{% endhighlight %}

# ARGUMENTS

*procs*
: An array of pmix_proc_t structures defining the processes to be aborted. A _NULL_
for the proc array indicates that all processes in the caller's
nspace are to be aborted. A wildcard value for the rank in any structure indicates
that all processes in that nspace are to be aborted.

*nprocs*
: Number of pmix_proc_t structures in the _procs_ array

*info*
: An array of pmix_proc_t structures defining the processes to be aborted. A _NULL_
for the proc array indicates that all processes in the caller's
nspace are to be aborted. A wildcard value for the rank in any structure indicates
that all processes in that nspace are to be aborted.

*ninfo*
: Number of pmix_info_t structures in the _info_w array

# DESCRIPTION

Execute a blocking barrier across the processes identified in the
specified array. Passing a _NULL_ pointer as the _procs_ parameter
indicates that the barrier is to span all processes in the client's
namespace. Each provided pmix_proc_t struct can pass PMIX_RANK_WILDCARD to
indicate that all processes in the given namespace are
participating.

The info array is used to pass user requests regarding the fence
operation. This can include:

(a) PMIX_COLLECT_DATA - a boolean indicating whether or not the barrier
    operation is to return the _put_ data from all participating processes.
    A value of _false_ indicates that the callback is just used as a release
    and no data is to be returned at that time. A value of _true_ indicates
    that all _put_ data is to be collected by the barrier. Returned data is
    cached at the server to reduce memory footprint, and can be retrieved
    as needed by calls to PMIx_Get(nb).

    Note that for scalability reasons, the default behavior for PMIx_Fence
    is to _not_ collect the data.

(b) PMIX_COLLECTIVE_ALGO - a comma-delimited string indicating the algos
    to be used for executing the barrier, in priority order.

(c) PMIX_COLLECTIVE_ALGO_REQD - instructs the host RM that it should return
    an error if none of the specified algos are available. Otherwise, the RM
    is to use one of the algos if possible, but is otherwise free to use any
    of its available methods to execute the operation.

(d) PMIX_TIMEOUT - maximum time for the fence to execute before declaring
    an error. By default, the RM shall terminate the operation and notify participants
    if one or more of the indicated procs fails during the fence. However,
    the timeout parameter can help avoid "hangs" due to programming errors
    that prevent one or more procs from reaching the "fence".


# RETURN VALUE

Returns PMIX_SUCCESS on success. On error, a negative value corresponding to
a PMIx errno is returned.


# ERRORS

PMIx errno values are defined in `pmix_common.h`.

# NOTES


# SEE ALSO

[`PMIx_Put`(3)](pmix_put.3.html),
[`PMIx_Commit`(3)](pmix_commit.3.html),
[`PMIx_Constants`(7)](pmix_constants.7.html)


