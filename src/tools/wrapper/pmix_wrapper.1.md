# NAME

pmix_wrapper - Back-end PMIx wrapper command

# SYNOPSIS

```
pmix_wrapper [options]
```

# DESCRIPTION

`pmix_wrapper` is not meant to be called directly by end users. It is
automatically invoked as the back-end by the PMIx wrapper command
`mpicc.`

Some PMIx installations may have additional wrapper commands, and/or
have renamed the wrapper compilers listed above to avoid executable name
conflicts with other PMIx implementations.

# SEE ALSO

The following may exist depending on your particular PMIx installation:
`pmixcc`(1) and the website at https://pmix.org/.

# AUTHORS

The PMIx maintainers -- see https://pmix.org/ or the file AUTHORS.

This manual page was originally contributed by Dirk Eddelbuettel
<edd@debian.org>, one of the Debian GNU/Linux maintainers for Open
MPI, and may be used by others.
