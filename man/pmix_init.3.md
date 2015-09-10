---
layout: page
title: PMIx_Init(3)
tagline: PMIx Programmer's Manual
---
{% include JB/setup %}

# NAME

PMIx_Init - Initialize the PMIx Client

# SYNOPSIS

{% highlight c %}
#include <pmix.h>

pmix_status_t PMIx_Init(pmix_proc_t *proc);

{% endhighlight %}

# ARGUMENTS

*proc*
: Fabric endpoint on which to initiate atomic operation.

# DESCRIPTION

Initialize the PMIx client, returning the process identifier assigned
to this client's application in the provided pmix_proc_t struct.
Passing a parameter of _NULL_ for this parameter is allowed if the user
wishes solely to initialize the PMIx system and does not require
return of the identifier at that time.

When called, the PMIx client will check for the required connection
information of the local PMIx server and will establish the connection.
If the information is not found, or the server connection fails, then
an appropriate error constant will be returned.

If successful, the function will return PMIX_SUCCESS and will fill the
provided structure with the server-assigned namespace and rank of the
process within the application.

Note that the PMIx client library is referenced counted, and so multiple
calls to PMIx_Init are allowed. Thus, one way to obtain the namespace and
rank of the process is to simply call PMIx_Init with a non-NULL parameter.

## Atomic Data Types

Atomic functions may operate on one of the following identified data
types.  A given atomic function may support any datatype, subject to
provider implementation constraints.

*FI_INT8*
: Signed 8-bit integer.

*FI_UINT8*
: Unsigned 8-bit integer.

*FI_INT16*
: Signed 16-bit integer.

*FI_UINT16*
: Unsigned 16-bit integer.

*FI_INT32*
: Signed 32-bit integer.

*FI_UINT32*
: Unsigned 32-bit integer.

*FI_INT64*
: Signed 64-bit integer.

*FI_UINT64*
: Unsigned 64-bit integer.

*FI_FLOAT*
: A single-precision floating point value (IEEE 754).

*FI_DOUBLE*
: A double-precision floating point value (IEEE 754).

*FI_FLOAT_COMPLEX*
: An ordered pair of single-precision floating point values (IEEE
  754), with the first value representing the real portion of a
  complex number and the second representing the imaginary portion.

*FI_DOUBLE_COMPLEX*
: An ordered pair of double-precision floating point values (IEEE
  754), with the first value representing the real portion of a
  complex number and the second representing the imaginary portion.

*FI_LONG_DOUBLE*
: A double-extended precision floating point value (IEEE 754).

*FI_LONG_DOUBLE_COMPLEX*
: An ordered pair of double-extended precision floating point values
  (IEEE 754), with the first value representing the real portion of
  a complex number and the second representing the imaginary
  portion.



# RETURN VALUE

Returns PMIX_SUCCESS on success. On error, a negative value corresponding to
a PMIx errno is returned. PMIx errno values are defined in
`pmix_common.h`.

# ERRORS

*-FI_EOPNOTSUPP*
: The requested atomic operation is not supported on this endpoint.

*-FI_EMSGSIZE*
: The number of atomic operations in a single request exceeds that
  supported by the underlying provider.

# NOTES


# SEE ALSO

[`fi_getinfo`(3)](fi_getinfo.3.html),
[`fi_endpoint`(3)](fi_endpoint.3.html),
[`fi_domain`(3)](fi_domain.3.html),
[`fi_cq`(3)](fi_cq.3.html),
[`fi_rma`(3)](fi_rma.3.html)
