# NAME

pquery - Request information from the system

# SYNOPSIS

```
pquery [ options ] [ keys ]
```

# DESCRIPTION

`pquery` will request and output the specified information. This requires that `pquery` connect to an appropriate server - the tool will automatically attempt to do so, but options are provided to assist its search or to direct it to a specific server.

Keys passed to `pquery` may optionally include one or more qualifiers, with the
individual qualifiers delimited by semi-colons. For example:
```
PMIX_STORAGE_XFER_SIZE[PMIX_STORAGE_ID=lustre1,lustre2;PMIX_STORAGE_PATH=foo]
```
# OPTIONS

 `pquery` supports the following options:

```
-h|--help                This help message
-n|--nspace <arg0>       Specify server nspace to connect to
-p|--pid <arg0>          Specify server pid to connect to
   --system-server       Specifically connect to the system server
   --system-server-first Look for the system server first
   --uri <arg0>          Specify URI of server to connect to
-v|--verbose             Be Verbose
```

# AUTHORS

The OpenPMIx maintainers -- see
[https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix)
or the file `AUTHORS`.
