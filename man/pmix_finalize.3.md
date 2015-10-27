---
layout: page
title: PMIx_Finalize(3)
tagline: PMIx Programmer's Manual
---
{% include JB/setup %}

# NAME

PMIx_Finalize - Finalize the PMIx Client

# SYNOPSIS

{% highlight c %}
#include <pmix.h>

pmix_status_t PMIx_Finalize(void);

{% endhighlight %}

# ARGUMENTS


# DESCRIPTION

Finalize the PMIx client, closing the connection with the local PMIx server.

# RETURN VALUE

Returns PMIX_SUCCESS on success. On error, a negative value corresponding to
a PMIx errno is returned.

# ERRORS

PMIx errno values are defined in `pmix_common.h`.

# NOTES


# SEE ALSO

[`PMIx_Init`(3)](pmix_init.3.html)
